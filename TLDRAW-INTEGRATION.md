# tldraw + spytial Integration — Architecture

Whiteboards as first-class Emacs buffers, on the web display backend.
A [tldraw](https://tldraw.dev) infinite-canvas board is overlaid on the
window of any buffer in `tldraw-mode`; the board's content lives in the
buffer as JSON (so it saves, diffs, version-controls, and survives hot
reloads), it is driven by Emacs keybindings, it theme-syncs with Emacs,
and [spytial-core](https://github.com/sidprasad/spytial-core) can lay
out relational data (filesystems, graphs) onto it.

Companion to [WEB-DISPLAY-ARCHITECTURE.md](WEB-DISPLAY-ARCHITECTURE.md)
and [PREEMPTIVE-THREADS-ARCHITECTURE.md](PREEMPTIVE-THREADS-ARCHITECTURE.md).

## Layers

```
 Emacs (tldraw-mode buffer; .tldr text = tldraw snapshot JSON)
   │  per-window  "tldraw":"<board-id>"   (webterm.c, mirrors "webview")
   │  outbound    web-tldraw-send(json)   (webterm.c primitive)
   │  inbound     tldraw_* JSON  ──►  WEB_EVT_TLDRAW (web_event_loop.c, async IO
   │                                   thread) ──► Vweb_tldraw--pending queue
   │                                   ──► drain timer ──► web-tldraw--dispatch
   ▼
 Proxy (unchanged NDJSON pipe)
   ▼
 Client: state.js relays tldraw_* to board components; App.jsx <TldrawLayer>
         mounts a <TldrawBoard> per board-window, positioned by px/py/pw/ph.
         Editor API applies commands; spytial-core computes layouts.
```

**Source of truth is the Emacs buffer.** The `.tldr` buffer text is the
tldraw store snapshot (`editor.getSnapshot()` → `{document, session}`).
The client is a view/editor. An **epoch** per board prevents echo loops:
`tldraw_load` (Emacs→client) bumps the epoch; `tldraw_snapshot`
(client→Emacs, debounced) is stamped with the epoch it descends from,
and Emacs ignores stale echoes. Emacs only sends a load on board
`ready`, `revert`, or an explicit API edit — never on buffer
modification — so there is no loop.

**Keyboard-first.** The overlay is `pointer-events:none` (like
`.webview-layer`), so Emacs keeps the keyboard; `tldraw-mode` turns keys
into `tldraw_cmd` messages and the client drives tldraw purely through
the `editor` ref. `m` toggles `mouse-enabled` for direct mouse use.

## Wire messages (`tldraw_*`)

Emacs → client: `tldraw_load` {board, epoch, snapshot}, `tldraw_cmd`
{board, verb, args}, `tldraw_theme` {colorScheme, bg, palette},
`tldraw_node_text` {board, id, text}, `tldraw_layout` {board, data, spec}.

Client → Emacs: `tldraw_event` {board, event: ready|selection|created|
edit-begin|edit-end, …}, `tldraw_snapshot` {board, epoch, snapshot}.

`tldraw_cmd` verbs (see `web-client/src/tldraw/navigation.js`):
`focus-move`, `focus-seq`, `focus-first`, `select`, `enter-edit`,
`exit-edit`, `node-text`, `create-node`, `create-text`, `create-edge`,
`delete`, `nudge-bend`, `mouse`, `camera`.

## Files

- C: `src/webterm.c` (`web_capture_tldraw`, `"tldraw"` field,
  `web-tldraw-send`, `web-tldraw--take-pending`, `web-tldraw--pending`),
  `src/webterm.h` (`tldraw_id`), `src/web_event_loop.{c,h}`
  (`WEB_EVT_TLDRAW` + heap `payload`; **this is the live inbound path in
  async mode** — `web_read_socket`'s branch is the sync fallback).
- Elisp: `lisp/web-tldraw.el` — `tldraw-mode`, sync engine, keymap,
  edge mode, in-node editing, theme sync, public API, `web-tldraw-filesystem`.
- JS: `web-client/src/tldraw/{TldrawLayer,TldrawBoard}.jsx`,
  `navigation.js`, `theme.js`, `spytial.js`; hooks in `state.js`,
  `App.jsx`, `emacs.css`.
- Deps: `tldraw`, `js-yaml`, `spytial-core` (+ `tslib`). spytial-core
  ships a global IIFE bundle (its package `.` export is not
  ESM-importable), so it is copied from `node_modules` to
  `public/vendor/spytial-core.global.js` by the `vendor` npm script
  (run automatically via `predev`/`prebuild`) and loaded via a script
  tag at runtime.  `public/vendor/` is gitignored — regenerated, not
  committed.

## Usage

```elisp
(require 'web-tldraw)
(web-tldraw-create)                 ; scratch board
(web-tldraw-create "diagram.tldr")  ; file-backed (C-u for interactive prompt)
(web-tldraw-filesystem "~/project") ; render a directory tree
;; Public API:
(web-tldraw-add-node "Hello" :x 0 :y 0 :id "shape:n1")
(web-tldraw-add-edge "shape:n1" "shape:n2")
(web-tldraw-spytial DATA YAML-SPEC) ; relational data -> laid-out board
```

Keys in `tldraw-mode`: `C-n/C-p/C-b/C-f` (or `n/p/b/f`) directional
focus; `C-M-n/C-M-p` / `TAB`/`S-TAB` cycle nodes; `C-l`/`M-l` recenter
the node (cycles center/top/bottom, like `recenter-top-bottom`); `RET`
run the node's action (else edit it; else commit an edge if an anchor is
set); `C-SPC` set edge anchor; `e` edit node; `a`/`t` add node/text; `d`
delete; `<`/`>` bend arrow; **arrow keys (or `M-arrows`) move the
selected node** (`S-arrows` = larger step; `web-tldraw-nudge-step` /
`-big-step`); `=` zoom-fit, `+`/`-` **zoom in/out**, `0` reset, `.`
center; `C-/` undo, `C-?` redo; `u` toggle UI; `m` toggle mouse; `C-g`
clear anchor. A freshly created node is centered in view.

The in-node edit buffer is Git-commit/magit-style: type the node text at
the top, an informational comment block (keybindings + board/node
context) follows, and lines starting with `web-tldraw-node-comment-start`
(default `# `) are stripped from the text. `C-c C-c` commits and returns
to the board, `C-c C-k` cancels.

### Node actions (RET) + metadata

Shapes carry arbitrary `meta` (set via `web-tldraw-add-node :meta` or
`web-tldraw-set-meta`); the client returns it in selection/created
events. `RET` runs `web-tldraw-node-action-functions` — an abnormal hook
called `(BOARD NODE-ID META TEXT)`, `run-hook-with-args-until-success`;
the first handler wins, else RET edits the node. `web-tldraw-node-meta`
reads a key from the selected node's meta. Add buffer-local handlers for
per-board behavior (this is how `web-tldraw-dired` makes RET unfold).

### UI customization

`web-tldraw-hide-ui` and `web-tldraw-grid` are defcustom defaults that
can be overridden buffer-locally (e.g. `(setq-local web-tldraw-hide-ui t)`
in a mode hook). `\\[web-tldraw-toggle-ui]` / `web-tldraw-toggle-grid`
toggle per-board. Sent as `tldraw_config`; the client hides
`.tlui-layout` via a CSS class (the `hideUi` prop doesn't react after
mount) and sets grid mode via `updateInstanceState`.

### web-tldraw-dired

`M-x web-tldraw-dired DIR` renders a directory as an unfoldable **indented
outline tree**: each entry sits at `x = depth * web-tldraw-dired-layer-width`
(default 280), `y = preorder-row * web-tldraw-dired-row-height` (default 60).
RET on a directory toggles expansion and rebuilds (`clear` verb + explicit
positions), keeping your place by re-selecting the toggled node; RET on a
file opens it. Arrows are off by default (`web-tldraw-dired-draw-edges`) —
the indentation conveys hierarchy without clutter. Built on the public API
(node `meta`, the node-action hook, hide-ui), so it's a worked example.
(An earlier version used `web-tldraw-spytial` for layout, but spytial's
qualitative output gave a noisy grid; explicit tree positions are cleaner.
`web-tldraw-spytial` remains available for relational data.)

RET on a **file** opens it via `web-tldraw-dired-find-file-function`
(default `find-file-other-window`; `find-file` for same-window). RET on
an **image** (`web-tldraw-image-extensions`) embeds it on the canvas
instead, via `web-tldraw-add-image` (file → base64 data URL → tldraw
image asset/shape, sized to the image, capped at 360px). Node labels
carry Unicode type icons via `web-tldraw-dired-icon-function` (default
`web-tldraw-dired-default-icon`; set it to integrate all-the-icons by
returning a glyph the board font renders). The selection is seeded to
the root on build so navigation/RET act on the node you're looking at.

### Appearance: font, corners, icons

Board text uses the **same font as the Emacs editor** — `theme.js`
`applyFont` copies the `.emacs-frame` computed `font-family` onto
tldraw's `--tl-font-{draw,sans,serif,mono}` CSS vars on the board
container (re-applied on theme changes). Nodes are created with geo
`dash: 'solid'` for **sharp (square) corners** rather than tldraw's
default hand-drawn rounded style. `web-tldraw-add-node :w :h` sizes a
node; `:meta` attaches data; `:dash` overrides the corner style.

### Mouse mode, UI, and the mode line

Boards are keyboard-first: the on-canvas tldraw UI (toolbars/menus) is
hidden by default and revealed only in **mouse mode** (`m` /
`web-tldraw-toggle-mouse`), which also enables pointer events. The mode
line shows `[kbd]`/`[mouse]`, `[edge]` while an edge anchor is set, and
`{selected node text}`. `web-tldraw-hide-ui` (default t) and
`web-tldraw-grid` are buffer-local-overridable; `u` toggles UI manually.

**Moving / bending**: the nudge keys are context-sensitive — arrow keys
(or `M-arrows`) **move** a selected node, but **bend** a selected arrow
(its single control point); `S-arrows` use a larger step; `<`/`>` bend,
`|` straightens.

**Theme**: `web-tldraw-sync-theme` (hooked to `enable-theme-functions`)
drives the dark/light scheme (user prefs), the canvas background, the
editor font, and tldraw's UI-chrome `--tl-color-*` vars
(primary/selected/selection-fill/text/grid/divider) from the active
faces. **Limitation**: tldraw 5's named *shape* colors (blue/green/…)
are JS-computed inline SVG with no public export or CSS hook, so they
can't be remapped to arbitrary theme faces — only chrome + scheme + bg
+ font track the theme.

## spytial layout

`generateLayout()` returns `{layout: {nodes, edges, constraints,
groups, …}}` — **qualitative** (LayoutNode has no x/y; edges carry full
source/target node objects). `spytial.js` solves positions from the
ordering constraints (longest-path layering; grid fallback) and builds
tldraw geo+arrow shapes. **Groups** (`layout.groups`, each `{name,
nodeIds}`) render as labelled dashed containers behind their members —
note the `group` directive must sit under `constraints:` (not
`directives:`) for spytial to emit it. The built-in positioner does not
*cluster* grouped nodes (it reads only Top/Left constraints), so group
boxes can be loose; tight clustering + full WebCola fidelity is future
work.

## Notes / limitations

- Board content uses `editor.getSnapshot()` (`{document, session}`);
  programmatic store changes don't reliably flush tldraw's document
  listener, so each applied command also schedules an explicit snapshot
  push (mouse edits use the store listener).
- The drain timer (0.1s) runs only while board buffers exist (preserves
  0% idle CPU otherwise).
- Boards are pooled/capped (4) in `TldrawLayer`; evicted boards reload
  from Emacs on reappearance (content is in Emacs, so this is safe).
- In-node editing currently mirrors a real Emacs buffer's text live onto
  the shape; rendering an actual Emacs window *inside* a shape rect is a
  future north-star.
