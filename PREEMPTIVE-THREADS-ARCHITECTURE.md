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
  on attach it requests a full redraw.  Result: zero client
  disconnects across a reload.
- **slop-reload.sh** — rebuild + `emacsclient -s web-emacs -e
  '(session-reload)'`, falling back to marker + kill if wedged.

Why not pdumper: dump files are fingerprint-locked to the binary that
wrote them, so state cannot cross a rebuild boundary as a memory
image; and pdumper can't capture window-system state, fds, or
subprocesses.  Session state crosses at the Lisp level; long-lived
subprocesses (the `claude` CLI) should live in tmux (slop-term.el) and
get reattached by desktop-restore handlers.

**Rule: always `emacsclient -s web-emacs`.**  The default socket name
belongs to your daily Emacs; the web instance must never be addressed
(or addressable) through it.

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
- web-term/claude-in-Emacs terminal: lisp/web-term.el is a frontend to
  a `web-vterm-*` libvterm C layer that does not exist yet.

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
