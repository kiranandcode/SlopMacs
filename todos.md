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
