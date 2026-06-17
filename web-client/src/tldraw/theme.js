/* Theme bridge: map the active Emacs theme onto a tldraw board.

   Emacs sends `tldraw_theme' messages built by `web-tldraw-sync-theme'
   (web-tldraw.el):

     { type:"tldraw_theme", board?, colorScheme:"dark"|"light",
       bg:"#rrggbb", fg:"#rrggbb", palette:{ blue:"#..", ... } }

   - colorScheme drives tldraw's own dark/light theme via user prefs.
   - bg recolors the canvas background (--color-background).
   - the Emacs palette/fg recolor tldraw's UI *chrome* (grid, selection,
     accent, text, dividers) via its --tl-color-* CSS variables, so the
     board's furniture tracks the theme.

   Note: tldraw 5's named *shape* colors (blue/green/...) are JS-computed
   inline SVG with no public export or CSS hook, so they cannot be
   remapped to arbitrary theme faces at runtime — only the chrome and
   dark/light scheme follow the theme.  */

/* Board font.  By default boards inherit the editor font via CSS
   (.tl-container reads var(--emacs-font-family)), so no JS is needed to
   match the editor — and if the editor font changes, boards follow.
   FAMILY (from Emacs `web-tldraw-set-font') is an optional runtime
   override applied through --slop-board-font on :root; passing a falsy
   value clears the override and reverts boards to the editor font.  */
export function applyFont (editor, family) {
  const root = document.documentElement;
  if (typeof family === 'string' && family.length) {
    root.style.setProperty('--slop-board-font', family);
  } else if (family === '' || family === null) {
    root.style.removeProperty('--slop-board-font');
  }
}

export function applyTheme (editor, msg) {
  if (!editor || !msg) return;

  if (msg.colorScheme === 'light' || msg.colorScheme === 'dark') {
    try {
      editor.user.updateUserPreferences({ colorScheme: msg.colorScheme });
    } catch (e) { /* older API: ignore */ }
  }

  applyFont(editor, msg.font);

  const container = editor.getContainer?.();
  if (!container) return;

  /* Canvas background blends with the Emacs frame.  */
  if (msg.bg) container.style.setProperty('--slop-canvas-bg', msg.bg);

  /* Recolor tldraw's UI chrome from the theme: an accent (first
     available palette face) drives selection/primary; fg drives text;
     a muted face drives the grid/dividers.  These are real CSS vars
     (--tl-color-*) that tldraw reads live, so no repaint nudge needed.  */
  const pal = (msg.palette && typeof msg.palette === 'object') ? msg.palette : {};
  const accent = pal.blue || pal.violet || pal.green || msg.fg;
  const muted = pal.grey || msg.fg;
  const set = (v, c) => { if (typeof c === 'string') container.style.setProperty(v, c); };
  set('--tl-color-primary', accent);
  set('--tl-color-selected', accent);
  set('--tl-color-selection-fill', accent);
  set('--tl-color-selection-stroke', accent);
  set('--tl-color-focus', accent);
  set('--tl-color-text', msg.fg);
  set('--tl-color-grid', muted);
  set('--tl-color-divider', muted);
}
