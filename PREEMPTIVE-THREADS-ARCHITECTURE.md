# Preemptive Threads & Hot Reload — Architecture

Branch: `preemptive-threads`.  Companion to
[WEB-DISPLAY-ARCHITECTURE.md](WEB-DISPLAY-ARCHITECTURE.md) (the display
tier).  This document covers the concurrency retrofit of the Emacs core
and the hot-reload development loop built on top of it.

**Goal: elisp evaluation can never hang the editor.**  A slow command
(package refresh, big parse) keeps rendering its progress, C-g always
works, and after a detach the user keeps editing in other buffers while
the command finishes in the background.

The key insight: this needs **preemption, not parallelism**.  One core,
time-sliced, is sufficient — so the stock runtime, GC, and `.elc`
ecosystem are kept (see "Why not another runtime" at the end).

```
┌────────────────────────────────────────────────────────────────────┐
│ OS main thread = UI THREAD                                         │
│   redisplay every 30ms while any command runs; sleeps otherwise.   │
│   Owns signal delivery.  Runs no user Lisp (only redisplay Lisp:   │
│   jit-lock, mode-line — same Lisp sit-for runs today).             │
├────────────────────────────────────────────────────────────────────┤
│ command-executor THREAD (a real Lisp thread, thread.c)             │
│   runs the ENTIRE command loop: startup files, commands, hooks,    │
│   recursive edits, minibuffer.  Yields the global lock every       │
│   thread-slice-ms at quit-check safe points.                       │
│       │ command runs > thread-detach-ms?                           │
│       ▼                                                            │
│   DETACH: spawn a fresh executor that takes over input; this       │
│   thread finishes its command in the background, then exits.       │
├────────────────────────────────────────────────────────────────────┤
│ web I/O THREAD (web_event_loop.c)  — unchanged from display tier   │
└────────────────────────────────────────────────────────────────────┘
```

## Stage 1 — Preemptive time-slicing

`maybe_quit` (lisp.h) doubles as the scheduling point: every 64th call
funnels into `thread_consider_preempt` (thread.c).  Once the running
thread has held the global lock longer than `thread-slice-ms`
(default 15), it parks via the same `flush_stack_call_func` path as
`thread-yield`, so the GC can scan the parked stack.

Because `maybe_quit` sits on bytecode backward branches, funcall entry,
and inside the long loops of C primitives (regex included), **C
primitives preempt too** — a single 6-second `string-match` yields
every slice.  A primitive that never calls `maybe_quit` blocks until it
returns (rare; add `maybe_quit` calls case by case).

Preemption is suppressed inside critical sections: `inhibit-quit`,
GC, redisplay, blocked input.  While a thread is parked mid-primitive
its C frames may hold raw string-data pointers, so `sweep_strings`
skips small-string compaction while `thread_preempt_parked > 0`
(stock `probably_quit` inhibiting GC at these points confirms the
hazard).

Knobs: `thread-preemption` (default t), `thread-slice-ms` (15).

## Stage 2 — Executor / UI thread split

At the top-level `Frecursive_edit` call (emacs.c), interactive
sessions hand the command loop to a dedicated Lisp thread
(`start_command_executor`) and the OS main thread enters
`ui_thread_loop`: redisplay on a 30ms cadence while
`command_executor_busy_p()`, long sleeps otherwise (new
`sys_cond_timedwait` in systhread.c).  Disable with the
`EMACS_NO_COMMAND_EXECUTOR=1` environment variable.

Existing `thread.c` machinery already provides per-thread context
(own C stack, specpdl swap, current-buffer, match-data, per-thread
`getcjmp`), which is why this works without a runtime replacement.

**C-g routing** (the subtle part):
- `probably_quit` (eval.c): non-executor threads defer `Vquit_flag`
  (kill-emacs honored anywhere) so UI-thread jit-lock can't steal C-g.
- `handle_interrupt` (keyboard.c): never longjmps into another
  thread's `read_char`.
- `handle_interrupt_signal`: with no tty terminal, stock Emacs treated
  SIGINT as "please die" — fatal for web sessions where the proxy
  forwards C-g as SIGINT.  With the executor active it only sets
  flags (async-signal-safe: it must not touch the global lock —
  `maybe_reacquire_global_lock` clobbers `current_thread`).
- In proxy-attached mode interrupts arrive as data; the web I/O
  thread raises SIGINT itself at parse time, so C-g works while the
  executor is busy (the event queue is only drained on input reads).

## Stage 3 — Detach-on-slow

When the foreground command exceeds `thread-detach-ms` (default 100)
at a preemption checkpoint, `command_executor_detach` spawns a
`command-executor--resume` thread that immediately owns input; the old
thread finishes its command in the background.  When done, it throws
`command-executor-detached` (caught in the thread entry DEFUNs) and
exits — clean unwind through `recursive-edit` at any depth, since the
level ++/-- commute.

Constraints: only top-level commands detach (`command_loop_level == 0`,
no minibuffer, no keyboard macro).  Busy state is a count
(`command_busy_count`, per-thread `in_command` flag, balanced even on
nonlocal exit and thread death).  The executor reference is a
staticpro'd Lisp object so GC can't reap a dead executor's
`thread_state`.

Semantics: C-g is consumed by the *foreground* executor; detached
background commands survive it (killing them is future work —
`thread-signal` UX).  Interleaving only ever touches commands that are
already slow; fast commands run serialized exactly as today.

Knobs: `thread-detach-commands` (default t), `thread-detach-ms` (100).

Verified (test/web-e2e/): 50 display updates streamed during one
command; text typed 600ms into a 2s command rendered 1.4s before the
command finished; C-g mid-loop; stacked double-detach; minibuffer and
recursive edits on the executor; 38/38 stock thread tests; 0% idle CPU.

## Hot reload (the dogfood loop)

Claude (or you) edits C → `./slop-reload.sh` → the live instance saves
its session and is replaced by the rebuilt binary, with the
browser/Electron window keeping its WebSocket throughout.

Pieces:
- **lisp/session-reload.el** — `session-reload` saves modified files,
  visited-file list + points (desktop.el), window layout
  (`window-state-get`), and non-file buffer contents, then
  `(kill-emacs 42)`.  On startup, `session-reload-init` (invoked by
  the wrapper) claims the **`web-emacs`** server socket and restores a
  pending session.
- **web-display/emacs-wrapper.sh** — restarts Emacs on exit code 42 or
  the `/tmp/emacs-reload-requested` marker; same path = rebuilt binary.
- **Persistent proxy** — `emacs-web-display --emacs-port N` listens for
  Emacs to (re)attach over TCP (Emacs connects when
  `EMACS_WEB_EMACS_PORT` is set, else forks its own proxy as before).
  On detach it keeps the WebSocket clients and awaits the next Emacs;
  on attach it replays the client's last `font_metrics` and `resize`
  messages (cached from the live stream — the client only sends them
  on WebSocket open, which never recurs across an Emacs restart) and
  then requests a full redraw, so the fresh instance comes up at the
  window's real size instead of 80x25.  Result: zero client
  disconnects across a reload.
- **slop-reload.sh** — rebuild + `emacsclient -s web-emacs -e
  '(session-reload)'`, falling back to marker + kill if wedged.

Why not pdumper: dump files are fingerprint-locked to the binary that
wrote them, so state cannot cross a rebuild boundary as a memory
image; and pdumper can't capture window-system state, fds, or
subprocesses.  Session state crosses at the Lisp level; long-lived
subprocesses (the `claude` CLI) live in tmux (slop-term.el) and are
reattached from an explicit registry (`slop-terms.eld`, written by
session-reload-save) — deliberately not desktop.el handlers, whose
save/restore round-trip silently drops the buffer if one restore
fails.

## web-term: terminals / Claude Code in Emacs

`src/webvterm.c` (built when configure finds libvterm; `HAVE_VTERM`)
exposes `web-vterm-*` primitives: a VT-screen parser whose cell grid
is rendered into a normal Emacs buffer as propertized text (anonymous
plist faces, run-length batched per row).  Emacs redisplay's own
row diffing keeps browser traffic incremental even though each update
re-renders the whole grid.  lisp/web-term.el drives it: a PTY process
(`stty onlcr; exec …` — Emacs clears ONLCR on PTYs) feeds
`web-vterm-write`, then `web-vterm-update` syncs the buffer and the
cursor follows `web-vterm-get-cursor`.  Keys are encoded by libvterm
(`web-vterm-key-input`), so arrows/ctrl/function keys and bracketed
paste are exact.

lisp/slop-term.el layers tmux on top: `M-x slop-term NAME` attaches
`tmux new-session -A -s NAME`, so the process (e.g. the `claude` CLI)
survives Emacs restarts.  Across a hot reload the running Claude Code
conversation is preserved end-to-end: tmux keeps the process, the
registry reattaches the buffer, and the TUI re-renders into it.
Tests: test/webvterm-batch-test.el (18 primitive checks, batch).

**Rule: always `emacsclient -s web-emacs`.**  The default socket name
belongs to your daily Emacs; the web instance must never be addressed
(or addressable) through it.

## web-webview: browser views embedded in buffers

Because the display tier is a browser, a buffer can host live web
content.  Any buffer whose buffer-local `web-webview-url` is a string
gets an iframe overlaid on its window's body by the client
(`WebviewLayer` in App.jsx): webterm.c captures the URL during
redisplay into the per-window JSON (`web_capture_webview`), the
client positions an iframe from the window's pixel geometry, and the
mode line stays visible beneath it.  The field is always emitted
(empty = none) so switching the window's buffer clears the overlay.
lisp/web-webview.el provides `web-webview-open URL` and
`web-org-roam-graph` (org-roam-ui's interactive graph in a buffer).
Webview buffers survive hot reloads: session-reload saves the URL
plus an optional `web-webview-revive-function` — org-roam-ui's
server dies with the old Emacs, so the graph buffer revives by
re-running `web-org-roam-graph`, which restarts it.

Iframes are pooled by URL on the client: when no visible window
shows a webview (buffer buried, tab switched), its iframe is
`display:none`-hidden, not unmounted, so the embedded app keeps
running like a background browser tab and reappears without a
reload.  Least-recently-visible entries are evicted past a small
cap; only eviction (or a page reload) restarts the app.

Input note: keys/clicks inside the iframe belong to the embedded
page (panning the graph); click Emacs text or a mode line to hand
input back.

Related C fix: `tty_frame_geometry` (term.c) used to `emacs_abort`
via the `FRAME_TTY` macro when any Lisp called
`frame-geometry`/`frame-edges` on a web frame (frame.el's dispatch
falls through to tty-frame-* for unknown window systems) — opening
the graph crashed Emacs through this path.  Web frames now take the
tty geometry path, which only uses generic frame fields.

## Electron

`web-client`: `npm run build` (vite) then `npx electron .`.  Electron
spawns the persistent proxy and Emacs-under-wrapper (your full init),
and loads the built client.  `web-client/native/fn_key.m` is an
NSEvent local monitor that rewrites the macOS Fn/Globe modifier to
Option before Chromium sees it (skipping keys that carry the Function
flag naturally); build with
`npx node-gyp rebuild --runtime=electron --target=<ver> --arch=arm64
--dist-url=https://electronjs.org/headers`.

First boot under a full user config can take minutes (package manager
rebuilds) and may prompt in the minibuffer (e.g. vterm offering to
compile its module) — the window shows the prompt; startup proceeds
once answered.  `/tmp/emacs-session-reload-status` records how far the
wrapper's init got, which is how you debug a headless instance.

## Rendering-artifact postmortem (2026-06-12)

One day of artifacts — duplicate blocks, ghost lines, half-window
blanking, scroll clipping — all traced to a single architectural
tension: **the client keys lines by Emacs matrix row index but paints
them at `pixel_y`**, and partial updates silently assume the two
never diverge.  They diverge under variable-height rows (images) and
under scrolls.  Record of what was tried, in order:

1. ~~Drop `pixel_y` from scroll-moved lines, let "the following
   frame_update" re-supply it~~ — **failed**: moved rows are exactly
   the rows Emacs never resends (that's the point of the scroll
   optimization).  Fallback `row*charH` painted ghost copies at
   compacted positions.
2. Scroll messages now carry `delta_px` (`run->desired_y -
   current_y`, webterm.c `web_scroll_run`) and the client translates
   moved lines' `pixel_y`.  Necessary but not sufficient.
3. ~~Per-update eviction (fresh rows evict overlapping stale
   rows)~~ — **failed for two paths**: rows arriving in the same
   flush protect each other, and scroll translations create overlaps
   with rows that were never "fresh".  Interleaved
   content-vs-empty-row families recurred when scrolling.
4. Within-flush inconsistency fixed server-side: every captured line
   gets a sequence stamp (`json_line.seq`); at flush, the older of
   any pixel-overlapping pair is dropped (a mid-cycle scroll reuses
   row indices, so the early capture is stale — the matrix never
   truly holds overlapping rows).
5. **Final client invariant**: `_dedupeOverlaps` (state.js) runs
   after *every* mutation — frame_update merge and scroll translation
   alike — walking lines in pixel order and dropping the lower-gen of
   any overlapping pair.  Evicted lines push their exact strip onto
   the window's `vacated` list, which the renderer erases *at those
   pixels* (the row-index fallback erases the wrong place).
6. Half-window blanking ("text clips halfway"): the renderer "erases"
   data-less row indices at `row*charH` **every frame** (gen -1 is
   never cached).  With client charH measured at 19 (fonts not loaded
   at measure time) vs Emacs's 22px rows, indices 28-42 erased pixels
   532-837 — permanently stomping the lower half of real content.
   Fix: erase passes skip strips occupied by any pixel-positioned
   line.  *Do not reintroduce unconditional erases keyed by row
   index.*
7. Mode-line flicker: the partially-visible bottom row painted into
   the mode-line strip and was overpainted a moment later; a
   desynchronized canvas presents mid-sequence.  Text rows now clip
   at the mode-line top (drawLine/drawLineBody).

Still open: the font-metric skew itself — re-measure on
`document.fonts.ready` and re-send `font_metrics`/`resize` if
changed.  The invariants above make the skew harmless to row
placement, but cursor pixel math and the atlas still use the early
measurement.

Debugging tools that cracked it (use them first next time):
- Canvas snapshot through the debug REPL: draw the canvas into a
  640px offscreen, `toDataURL`, decode locally — ground truth of what
  the user sees, without touching their screen.
- `getImageData` lit-pixel band sampling to test "is anything painted
  at row N" without eyeballing.
- `window._renderer` (exposed by Frame.jsx) for gen-cache and
  geometry introspection; `_state.windows` dumps for the line maps.
- The reliable repro was `dashboard-refresh-buffer` with the PNG
  banner (image row shifts every row's pixel position).

Related fixes the same day: cursor column math (x is already
text-area-relative — subtracting the window origin pushed cursors
off-screen in side-by-side splits); `dpyinfo->highlight_frame` was
never set so every cursor rendered hollow; the proxy now replays
cached `font_metrics`/`resize`/`focus` to a (re)attaching Emacs —
without that, a fresh instance sat at 80x25, unfocused, cursorless.
`tty_frame_geometry` aborted (via the FRAME_TTY macro) when any Lisp
called `frame-geometry` on a web frame; web frames now take that path
legitimately.

## Executor robustness postmortem (2026-06-14)

A 100%-CPU hang that C-g could not break, traced to the executor
architecture and fixed in three places.

**Root cause — `thread-buffer-killed` injected into the command loop.**
`thread_all_before_buffer_killed` (thread.c) sends a
`thread-buffer-killed` error to *any* non-caller thread holding a
buffer that is being killed.  In stock Emacs the command loop is the
main thread, which is never targeted this way; here it is the
*executor* thread.  LSP (and anything else) constantly creates and
kills buffers (stderr, `*lsp-log*`, temporaries).  When one the
executor merely had current was killed by another thread, the
executor took a `thread-buffer-killed` error *inside the command
loop* — `cmd_error` printed it, the loop re-ran, the dead-buffer
state persisted, and Emacs spun forever at 100% with no way out.
Fix: the executor is now exempted (treated like the main thread —
silently switch buffers, never take the error).  See
`thread_all_before_buffer_killed`.

**The hole that made it unrecoverable.** With the executor active,
`handle_interrupt_signal` only ever set the quit flag and returned —
it never reached the `Qkill_emacs` path.  So a wedged executor
swallowed *every* C-g as just another "Quit" to print; no keystroke
could kill or break it, and only `kill -9` recovered (losing the
session, since `session-reload-save` can't run on a wedged executor).

**The escape hatch (validated).** After five rapid C-g's with no quit
being processed — the signature of a wedge — `handle_interrupt_signal`
sets `executor_break_requested`.  The executor consumes it at its next
quit checkpoint (`executor_take_break_request`, called from
`probably_quit`) and `Ftop_level()`s — abandoning the wedged command
and unwinding to the command loop, *keeping the session alive*.  The
count only climbs when quits aren't being processed, so normal C-g is
unaffected.  Tested: an `(while t (condition-case nil (signal 'error
nil) (error nil)))` timer wedged the executor at 94% CPU; 5× SIGINT
dropped it to 0% with the editor and the live LSP workspace intact.

Files: `src/thread.c` (exemption), `src/keyboard.c`
(`executor_break_requested`, `executor_take_break_request`, the count-5
escalation), `src/eval.c` (`probably_quit` consumes the request and
throws to top level), `src/lisp.h` (decl).

**Amplifier — `debug-on-error`.** `~/.emacs.d/.custom.el` has
`'(debug-on-error t)` (set via Customize, almost certainly by
accident).  On the executor every error then enters the debugger,
which does its own recursive edit and produces "No recursive edit is
in progress" loops — turning routine errors into catastrophes.
Recommend removing it.

## Uninterruptible-primitive postmortem (2026-06-17)

A 100%-CPU spin (state `RN`) that **neither preemption nor the 5×C-g
escape hatch could break** — only `kill -9` + a wrapper restart
recovered it (losing in-memory state since the last session save).

Root cause class: both preemption (Stage 1) and the escape hatch
(2026-06-14 postmortem) hinge on the running thread reaching
`maybe_quit` / `probably_quit`.  Those checkpoints sit on bytecode
backward branches, funcall entry, and inside the loops of *most* C
primitives — but **not all** (already noted in Stage 1: "a primitive
that never calls `maybe_quit` blocks until it returns").  When the spin
is inside such a primitive: the scheduler can't park it (no
checkpoint), `Vquit_flag` is set but never checked, and the five rapid
SIGINTs *do* set `executor_break_requested` but it can never be
consumed (consumption is in `probably_quit`).  So the editor's
"nothing can hang me" guarantee is really "**no elisp-level loop, and
no checkpoint-bearing primitive, can hang me**" — a single tight C
primitive lacking `maybe_quit` is outside it by construction.

Note the **async I/O thread does not help here**: it keeps display and
socket I/O live, but never runs Lisp, so it cannot interrupt a Lisp
computation.

Not pinpointed (the wedged state was lost on restart).  Ruled out:
`json-serialize` (it *errors* on circular input, doesn't loop) and the
elisp-value walker (`web-tldraw--value->instance`, cycle-safe by eq
identity).  It surfaced during heavy `web-tldraw` use.  Next time:
`sample <emacs-pid>` *before* recovering to capture the spinning C
stack and name the primitive; the fix is then to add a `maybe_quit`
to that primitive's loop (the Stage-1 prescription) or to bound the
input before calling it.

Minor: `web_event_loop.c` gained a `WEB_EVT_TLDRAW` event type (a
heap-`payload` carrier for inbound `tldraw_*` board messages); see
[TLDRAW-INTEGRATION.md](TLDRAW-INTEGRATION.md).

## Build note: image libraries (GIF)

The web configure line passed only protobuf's include/lib paths, so
Homebrew's giflib (keg-only / non-default prefix) was invisible and
Emacs built **without GIF support** (`image-types` lacked `gif`).
That broke anything loading a GIF — the dashboard banner *and* LSP
(its spinner/progress UI), the latter aborting `lsp` outright.  Fix:
configure with giflib's paths added —

    ./configure --with-web --without-ns \
      LDFLAGS="-L/opt/homebrew/opt/protobuf@21/lib -L/opt/homebrew/opt/giflib/lib" \
      CPPFLAGS="-I/opt/homebrew/opt/protobuf@21/include -I/opt/homebrew/opt/giflib/include"

giving `HAVE_GIF 1`.  If other optional image/codec libs ever look
missing, check whether their Homebrew prefix is on the configure
include/lib path before assuming the feature is unavailable.

## Blocking-wait postmortem (2026-06-17)

A `flyspell` `post-command-hook` calling a silent `ispell` froze the
editor at **0% CPU** — a *blocking* wait, not a spin.  Two real bugs
behind it, both now fixed:

1. **Detach was silently dead.**  Detach-on-slow only fires when
   `command_loop_level == 0`, but the *resume* executor (spawned by the
   first-ever detach) enters `Frecursive_edit` while the detaching
   thread hasn't unwound `command_loop_level` yet, so it runs at level
   **1**.  After the very first detach the condition never matched
   again — every later slow/blocking command froze the editor.  Fix:
   `executor_recursive_edit` records `executor_base_loop_level`
   (`command_loop_level + 1`); detach and the wait clamp compare against
   that baseline, not a hard-coded 0.

2. **Blocking waits never reached the detach checkpoint.**  The detach
   check lives in `thread_consider_preempt`, reached via `maybe_quit`
   — which a thread asleep in `pselect` doesn't call.
   `wait_reading_process_output` now (a) clamps the *foreground*
   executor's select timeout to `thread-detach-ms`
   (`thread_executor_wait_cap_ms`), so it revisits its loop-top
   checkpoint every ~100 ms, and (b) calls a dedicated,
   non-rate-limited `thread_consider_detach()` each iteration.  A
   command blocked in I/O now detaches on the same deadline as a
   CPU-bound one: a fresh executor takes input, the blocked command
   finishes in the background.  Already-detached background threads
   aren't clamped (`thread_executor_wait_cap_ms` returns -1 for them),
   so they sleep efficiently at 0% CPU instead of polling.

   Verified: a command blocked in `accept-process-output` on a silent
   subprocess detaches in ~100 ms; a second command runs on the fresh
   executor while the first stays blocked; threads settle back when the
   blocked wait returns; minibuffer + C-g unaffected.

Nested synchronous reads (a minibuffer / `read-char-choice` prompt —
e.g. lsp-mode's "import project?"): the wedge here is *detach* opening
a prompt on the background thread.  When a command detaches and then
reads input, the minibuffer it opens blocks on input the foreground
executor owns, and the global `minibuf_level`/`command_loop_level` it
bumps corrupt the foreground loop ("not in the most nested command
loop", C-g dead).  `read_minibuf` and `recursive_edit_1` now guard
their entry with `command_executor_exit_if_detached()` — a detached
background executor abandons (throws `command-executor-detached`)
before touching global state, so the editor stays responsive and the
background command is dropped.  Correct by construction; deterministic
reproduction in synthetic tests proved flaky, so treat end-to-end
behavior as pending real-world validation.  (Normal foreground
minibuffer + C-g are unaffected — verified.)  Per-trigger mitigation
also in place: `lsp-auto-guess-root t` so lsp never pops that prompt.

Also still open: a *persistently* hung subprocess in a
`post-command-hook` makes each command detach a fresh background thread
(they sleep, but accumulate) — bounding that is future work; the editor
stays responsive throughout.

## Known limitations / future work

(tracked in todos.md)
- Foreground-priority scheduling: background commands share slices
  equally; typing latency degrades under several CPU-bound detached
  commands.
- No way yet to kill a detached background command (thread-signal UX).
- Process filters/sentinels/timers don't yet route through the
  executor machinery.
- Cross-thread same-buffer mutation can invalidate a parked
  primitive's buffer-text pointers (unguarded; per-buffer work is
  safe).
- Shared-global `setq` races between a detached command and foreground
  editing are possible — the per-thread isolation covers let-bindings,
  buffers, match-data only.
- web-term: no scrollback yet (the buffer holds only the live screen;
  tmux history covers it via its own copy mode), no mouse reporting,
  and wide-char cursor column→buffer position math is approximate.

## Why not another runtime (Guile/Chez)

Tried; parked on branch `chez-runtime`.  Chez's copying GC is hostile
to 400k lines of C holding raw pointers (root-pinning is fragile and
`Slock_object` itself allocates), bootstrap ran interpreted with
dual-nil/`equal` correctness bugs, and any runtime swap loses pdumper
(load-time) while still not making elisp's dynamic-scope/buffer
semantics thread-safe.  Preemption on the stock runtime delivers the
actual goal — "nothing hangs the editor" — at a fraction of the cost,
reusing battle-tested per-thread context switching that `thread.c`
already had.
