 - want to setup an electron wrapper around this. it should be a macos .app that I can just double click and it handles everything

web-term / claude-code-in-emacs follow-ups (C layer DONE: src/webvterm.c):

 - scrollback: buffer only holds the live screen; wire sb_pushline into a
   scrollback ring rendered above the screen (tmux copy-mode covers it for now)
 - mouse reporting (claude code supports mouse; vterm_mouse_* are unbound)
 - cursor col->point math assumes 1 col == 1 char (wide chars drift)
 - incremental row updates via damage callbacks instead of full-grid re-render
   (fine so far: Emacs redisplay diffs rows anyway)

preemptive-threads follow-ups:

 - FOREGROUND-PRIORITY SCHEDULER: detached background commands currently get
   equal time slices with the foreground executor; under two CPU-bound
   background commands, typing latency was ~2s. Give the foreground executor
   priority (e.g. background threads get longer slices / yield more often, or
   foreground gets first claim on the lock after a yield).
 - way to kill a detached background command (thread-signal UX; maybe double C-g)
 - process filters/sentinels/timers through the executor machinery
 - dogfood magit on a real repo; decide same-buffer collision policy
 - pre-existing: synthetic font_metrics/resize from a client spins Emacs at
   99% CPU (resize loop; reproduces on stock build too)

things I want to fix:

 - cursor clicking is slightly off.
 - underlining words? (currently writing in scratch buffer, and all my words are being underlined for some reason)
 - image display (about emacs image displays but the text is not pushed beneath it)
 - webkit components integration - would love to have an emacs api to allow me to embed javascript widgets into an emacs buffer
   (ideally in a backwards compatible way, so vanilla emacs just sees the javascript)
   (FIRST CUT DONE: web-webview.el + iframe overlay — buffer-local web-webview-url
   renders any URL over the window body; M-x web-org-roam-graph embeds the
   org-roam-ui graph; survives hot reload via revive functions.  Remaining:
   arbitrary inline JS widgets *within* text flow, postMessage bridge
   iframe<->elisp, focus/keyboard handoff polish)
 - keyboard bindings (in the electron app; in firefox ofc not possible)
 - some commands don 't work the same in this build as they do on my default emacs... why??
   (partly answered: this build is Emacs 31 master, the daily Emacs is older, so
   upstream behavior changes surface here first — e.g. kill-this-buffer became
   menu-bar-only in 31 and had to be replaced with kill-current-buffer in
   init-dired/init-nonwm/init-wm.  Report further cases as they come up.)
 - some rendering glitches, when the buffer gets too long then the text disappears
   (LIKELY FIXED 2026-06-12: renderer erased data-less row indices at
   row*charH every frame — with image rows or a charH measured before fonts
   loaded, that band landed on real content and restomped it each pass.
   Erase passes now skip strips occupied by pixel-positioned lines.)
 - font-metrics skew: the client measures charH before webfonts finish
   loading (saw 19 vs Emacs's 22) — should re-measure on document.fonts.ready
   and re-send font_metrics+resize when it changed

## Found 2026-06-12 (scroll-artifact debugging session)
- [x] Reliable stale-row repro: `M-x dashboard-refresh-buffer` with the PNG
      banner (image insertion shifts every row's pixel_y) left a duplicate
      stale copy of the old rows.  FIXED (2026-06-12) by two mechanisms:
      (1) scroll messages now carry `delta_px` (run->desired_y - current_y)
      and the client translates moved lines' pixel_y instead of dropping it
      (moved rows are never resent — falling back to row*charH was wrong
      whenever row heights vary);
      (2) pixel-overlap eviction in state.js: lines are keyed by matrix row
      index but painted at pixel_y, so a line Emacs never resends can hold a
      pixel range now owned by fresh rows — fresh rows now evict any line
      they overlap, and the renderer erases the vacated strips exactly where
      the ghost painted.
- [ ] `make_multibyte_string` abort crashes (2x on 2026-06-12 13:47, see
      ~/Library/Logs/DiagnosticReports) — eval_sub → make_multibyte_string
      with bad length; likely a preemptive-threads string race.  Unfixed.
- [ ] Proxy footgun: a second Emacs connecting to `--emacs-port` silently
      displaces the live one (no handshake/refusal).  A stray test instance
      inheriting EMACS_WEB_EMACS_PORT killed the live display today.
      Proxy should refuse or require an explicit takeover flag.
- [ ] Emacs aborts (eassert in wait_reading_process_output) when its forked
      proxy exits after the last ws client disconnects — should detach
      gracefully instead.

## Visual polish: subpixel scroll + motion FX (2026-06-17)
- [x] True subpixel + momentum scrolling: webterm.c now builds the wheel
      event's arg as `list3(lines, Qt, pixels)` so `(nth 4 event)` is
      `(t . PIXELS)` — what `pixel-scroll-precision' reads (a plain fixnum
      arg landed at nth 3, leaving nth 4 nil → it fell back to line
      scrolling).  web-win.el enables `pixel-scroll-precision-mode`
      (+momentum/interpolate) and remaps C-v/M-v to
      `pixel-scroll-interpolate-{down,up}`.  Renderer (renderEngine.js)
      full-repaints a window whose top line has pixel_y<0 (vscroll active)
      to avoid scroll ghosting.  input.js keeps the sub-pixel wheel
      remainder and converts DOM_DELTA_LINE/PAGE wheels to pixels.
- [x] Motion FX overlay (web-client/src/fx.js): transparent canvas above
      the text, self-stopping rAF (0% idle).  Cursor comet trail + CSS
      caret glide (snap+beacon on big jumps); jump beacons + isearch
      flashes + smooth scroll-to-definition driven from lisp/web-fx.el via
      the generic `web-tldraw-send' channel ({"type":"fx",...}, absolute
      frame-pixel coords from posn-at-point + window-inside-pixel-edges).
- [x] LIVE-VERIFIED 2026-06-17 (hot-reloaded): wheel→precision vscroll
      works end-to-end (synthetic wheel-down → window-vscroll 4px → client
      top row pixel_y<0); FX flash hint reaches client.  GOTCHA for the
      debug REPL: zsh `echo` interprets `\n`, splitting NDJSON — use
      `String.fromCharCode(10)` for the message terminator.
- [x] Scroll ghosting (overlapping lines) FIXED: A3's lineGens.clear() was
      insufficient — a row repainted over the prior frame left a vscroll-px
      sliver behind.  Fix: when vscrolled, clear the whole window to bg
      before repainting all rows (renderEngine.js drawWindow).  Verified
      via canvas-snapshot→JPEG (static vscroll + mid-momentum both clean).
- [x] Uneven scroll / "only cursor line smooth, rest flickers" FIXED: two
      row-reuse optimizations bit-blit a block of rows while only changed
      rows pick up the sub-line vscroll → bulk steps by whole lines.
      (1) scrolling_window: webterm.c web_update_window_begin sets
      desired_matrix->no_scrolling_p when w->vscroll!=0 (runs before the
      check in update_window).  (2) try_window_id/try_window_reusing call
      scroll_run_hook directly (bypass no_scrolling_p) at vscroll==0
      line-boundary frames → disable via C specials inhibit-try-window-id/
      -reusing, dlet-bound ONLY around the precision-scroll fns in
      web-win.el (web--scroll-inhibit-reuse advice) so typing keeps its
      optimizations.  MUST use dlet (lexical-binding would make the let a
      no-op vs the C globals).  Verified: every frame has a single uniform
      inter-row pixel step (uniqSteps:[22]).
- [ ] Still worth doing if inference proves flaky under exotic layouts:
      emit `w->vscroll` as a per-window frame_update field instead of
      inferring vscroll from pixel_y<0.  Also: smooth-jump (C3) heuristic
      not yet stress-tested against misfires.

## Scroll/FX tweaks round 2 (2026-06-17)
- [x] C-v/M-v desync (buffer shifts a few px on next cursor move): page
      scroll left a fractional vscroll that redisplay zeroes on the next
      command (xdisp.c:20695).  Remapped C-v/M-v to web-scroll-up/down-page
      (web-win.el) = whole-page interpolate + web--snap-vscroll (snap
      residual vscroll to nearest line boundary).
- [x] C-v "lagging" + then typing/traces lag: ROOT CAUSE was
      pixel-scroll-precision-interpolation-between-scroll default 0.001s
      (~1000fps) — the interpolate loop calls (redisplay) every 1ms, and
      each WEB redisplay serializes the whole window over the WS → flood
      (~250 frames/page, drowning typing+FX).  Fixed: clamp to 0.016s
      (~60fps, ~19 frames/page).  Verified via new perf logger:
      window.__perfSummary() showed render avg 0.8ms, 2xC-v = 38 frames.
- [x] No cursor trail/beacon in the minibuffer: webterm.c emits per-window
      "mini":true (MINI_WINDOW_P); state.js carries win.mini; renderCursor
      skips trail/beacon when win.mini.
- [x] Smooth scroll for search/occur jumps: web-fx.el now tracks every
      window's pre-command start (pre-command-hook web-fx--record-starts)
      and smooth-scrolls on xref/imenu/next-error/consult after-jump hooks
      (works even when the jump lands in another window).
- [x] DEAD CODE removed: inhibit-try-window-id/-reusing are #ifdef
      GLYPH_DEBUG (off) → unbound, no-op.  The web--scroll-inhibit-reuse
      advice never did anything; scroll uniformity is all from no_scrolling_p.
- [x] Perf logger baked into web-client/src/perf.js: window.__perfSummary()
      / __perfReset() (render/fxtick/frames-from-emacs rates) for live
      diagnosis via the debug REPL.

## Scroll/FX tweaks round 3 (2026-06-17)
- [x] Smoother C-v/M-v: cadence 0.010s/0.22s total (~29 frames/page).
- [x] C-v "lines update when moving cursor" (canvas staleness, NOT state —
      state verified line-aligned & uniform gen): two parts. (1) Emacs:
      force-window-update after the snap so the settle redisplay resends
      the whole window (else far rows reused → stale). (2) Client: drawWindow
      keeps full-repainting ~280ms AFTER scroll stops (lastScrollAt) — the
      settle frame goes through the gen-cache which skips rows last painted
      at a mid-scroll position; the extra repaints catch the canvas up.
- [x] No cursor trail during mouse/C-v scroll (point pushed to stay
      on-screen smeared a trail at the window edge): renderCursor skips
      trail/beacon when win scrolled within 250ms (lastScrollAt).  Verified:
      0 trail calls during C-v, 4 for 4x C-n nav.
- [x] occur (M-s o) smooth scroll: advised occur-mode-goto-occurrence(+other-
      window/display) :after web-fx--after-jump (web-fx--after-jump now takes
      &rest).  max-screens cap raised 2.2 -> 5.0.  (User uses built-in
      isearch/occur; consult/vertico NOT installed.)

## Scroll/FX tweaks round 4 (2026-06-17)
- [x] Trackpad scroll had the same post-scroll staleness as C-v (leftover
      fractional vscroll, no snap).  Fix: web--vscroll-snap-on-idle advised
      onto pixel-scroll-precision-scroll-down/up (wheel + momentum + C-v all
      funnel through them) re-arms a 0.12s idle timer; once all scrolling
      stops it snaps vscroll to a line boundary + force-window-update.
- [x] Easing: C-v/M-v (web--scroll-page) rewritten as a hand-rolled loop
      with smoothstep ease-in-out velocity (accel→decel) at ~90fps, instead
      of pixel-scroll-precision-interpolate's linear-in-time glide.
      web-scroll-page-duration defcustom (0.30s).
- [x] Momentum: pixel-scroll-precision-use-momentum t +
      momentum-seconds 1.2; OS supplies trackpad inertia.

## Scroll/FX tweaks round 5 (2026-06-18)
- [x] REGRESSION fix: round-4's eased C-v loop used a fixed step count, so
      mid-scroll (smoothstep peak) deltas hit ~50px = several lines per
      redisplay → tripped Emacs row-reuse → client got overlapping/missing
      rows (uniqSteps [29,22,37], rowGap) whenever point moved.  Fix: size
      the step count from the distance so per-step delta ≈ 1 line (small
      deltas verified clean; linear loop and interpolate both clean too).
      KEY: small per-redisplay scroll deltas are mandatory for the web
      backend; large multi-line deltas corrupt the client render.  The
      frame double-buffer coalesces (~13 frames delivered for ~60 steps).
- [x] Trackpad: same small-delta property holds (high-res events) + the
      snap-on-idle settles cleanly.  Both C-v and trackpad verified
      uniqSteps [22], no gaps, including point-move cases.
- [ ] SPIN postmortem (2026-06-18): during heavy interactive testing
      (100+ overlapping synthetic scroll/key events + repeated live
      function redefinitions mid-scroll) the executor wedged at 100% CPU in
      command_loop_1 / redisplay (display_mode_element).  5xC-g escape hatch
      did NOT break it (stuck in redisplay, not a quit checkpoint).
      Recovered via `touch /tmp/emacs-reload-requested; kill -9 <pid>` →
      wrapper auto-restarts + session-reload-init restores.  A single clean
      C-v does NOT reproduce it (CPU returns to 0) — transient test-induced
      state, not the shipped code.  Still: avoid firing many overlapping
      scroll commands faster than they animate.

## Scroll/FX tweaks round 6 (2026-06-18)
- [x] C-v/M-v feel "too rigid" -> LERP-to-target glide (each step = 0.20 of
      remaining, capped ~1 line): fast start, decelerates into a soft stop
      (momentum), then web--settle eases leftover vscroll to a line boundary
      (no abrupt snap).  defcustoms web-scroll-page-lerp / -tick.
- [x] C-l animates (recenter-top-bottom advised :after web-fx--smooth-after-jump).
- [x] STALE-RENDER VERIFICATION TOOLS (user asked for a JS way to confirm):
      (a) canvas snapshot via toDataURL, chunked over debug REPL +
      base64-decoded (python /tmp/snap.py pattern) → view the jpg;
      (b) window.__rowHashes per-row pixel hash, diffed across a NO-SCROLL
      cursor move → clean iff only cursor-rows + mode-line changed.  Used it
      to CONFIRM the lerp scroll is clean (changedRows = [cursor rows, 44-45
      modeline] only; no far rows).  GOTCHA: don't sample a band the cursor
      sits in.

## Scroll round 7 (2026-06-18) — stabilized
- [x] C-v/M-v: REVERTED to pixel-scroll-precision-interpolate (round-3
      known-good).  User confirms "perfect".  The hand-rolled lerp + the
      animated web--settle corrupted the client (stale/offset rows) and the
      split-scroll advice hung in heavy buffers — all removed.
- [x] Reverted the dedup cursor-protection (it backfired: kept STALE cursor
      rows, causing "every line offset by one" after C-v).
- [x] Trackpad settle: web--vscroll-snap-on-idle does redisplay-while-
      vscrolled (force full resend) + instant snap on idle.  Helps but
      RESIDUAL: trackpad scroll-down that pushes cursor to the top row still
      has "refresh weirdness" when moving lines after — the deep pixel_y vs
      matrix-row-index divergence (see rendering-artifact postmortem).  Not
      fully fixed; would need a client scroll-handling rework.
- [x] C-l: FIXED.  Bug was `web-fx--smooth-after-jump` taking 0 args while
      the recenter-top-bottom :after advice passes the prefix arg →
      `wrong-number-of-arguments (0 . 0) 1` (debugger on every C-l).  Made it
      `(&rest _)`.  Now recenters with animation (no error).  (A recenter
      moving < ½ line won't visibly animate — nothing to move.)
- [ ] DEEP/UNSOLVED — scroll/cursor row divergence: trackpad scroll-DOWN
      that pushes the cursor to the top row leaves a stale/missing row
      (interactive "refresh weirdness" as you move the cursor after).  Fully
      documented in WEB-DISPLAY-ARCHITECTURE.md §11c.1.  Needs a client
      scroll/merge rework (reconcile rows by pixel_y, or backend per-row
      scroll-generation stamps) — NOT a point patch.  Don't re-try
      force-window-update / cursor-row dedup protection / hand-rolled loops
      (all tried, all failed/regressed).
- NOTE: synthetic scroll testing via the debug REPL is unreliable
      (find-file/point land in the wrong frame window — saw ptline 9 when set
      to 280).  Trust live user testing over synthetic repro.

## tldraw + spytial integration (2026-06-17)
- [ ] Uninterruptible 100%-CPU wedge during heavy web-tldraw use: neither
      preemption nor the 5xC-g escape hatch broke it (spin in a C
      primitive lacking maybe_quit; see PREEMPTIVE-THREADS-ARCHITECTURE.md
      "Uninterruptible-primitive postmortem").  Cause not pinpointed —
      next recurrence: `sample <emacs-pid>` BEFORE kill -9 to name the
      primitive, then add maybe_quit there or bound the input.
- [x] spytial group rendering (2026-06-17): the `group` directive must be
      under `constraints:` (not `directives:`) for spytial to emit
      layout.groups — fixed in web-tldraw--visualize-spec.  Client draws a
      dashed labelled box behind members; web-tldraw-visualize groups by
      leaf type (Symbol/Integer/...) with >1 atom.  NB: my positioner does
      not cluster grouped nodes (it only reads Top/Left constraints), so
      boxes can be loose; tight clustering would need honouring spytial's
      grouping constraints / a real solver.
- [x] Arrow control point (2026-06-17): nudge keys are context-sensitive
      — they bend a selected arrow (its single control point) and move a
      selected node; `|` straightens (bend 0); `<`/`>` still bend.
- [x] Theme chrome (2026-06-17): the Emacs theme now drives tldraw's
      --tl-color-{primary,selected,selection-fill,text,grid,divider} +
      canvas bg + font + dark/light scheme.  LIMITATION: tldraw 5's named
      *shape* colors (blue/green/...) are JS-computed inline SVG with no
      public export or CSS hook, so they cannot be remapped to arbitrary
      theme faces at runtime — only chrome + scheme follow the theme.
- [ ] web-tldraw-visualize: cyclic values conflict with the cdr-below
      orientation (spytial returns only a feasible subset); a proper
      ring layout needs honouring spytial's `cyclic` constraint in the
      client positioner (currently only reads Top/Left constraints).
- [x] Keyboard visual box-selection (2026-06-17): `v` enters
      web-tldraw-visual-mode at an anchor node; movement grows a
      rectangle to a cursor node and auto-selects nodes inside (count in
      mode line); RET keeps, d deletes, C-g cancels.  Multi-selection
      moves as a group via the nudge keys.  Geometry client-side
      (selectBox/visual-move in navigation.js).
- [x] Path-style visual selection (2026-06-17): `V` enters
      web-tldraw-visual-mode with style=path — each node moved onto is
      added to the selection (freeform set), vs `v`'s rectangle.  Shared
      visual-start/move verbs branch on the style; mode line shows
      [box N] / [path N].
- [x] Node clipboard + grouping + folding (2026-06-17): M-w/C-w/C-y
      copy/kill/yank selected nodes through the Emacs kill ring as
      portable text ({slopTldraw,content} JSON via
      getContentFromCurrentPage/putContentOntoCurrentPage; copy is a
      round-trip clip event); non-tldraw kills paste as a text node.
      `G' (or `g' in visual mode) wraps the selection in a tldraw frame;
      RET on a frame folds/unfolds by hiding members via the
      getShapeVisibility hook (meta.slopHidden) + collapsing the frame
      (no delete/recreate, so identity/bindings survive).
