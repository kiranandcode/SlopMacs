/* Theme bridge: map the active Emacs theme onto a tldraw board.

   Emacs sends `tldraw_theme' messages built by `web-tldraw-sync-theme'
   (web-tldraw.el):

     { type:"tldraw_theme", board?, colorScheme:"dark"|"light",
       bg:"#rrggbb", fg:"#rrggbb", palette:{ blue:"#..", ... } }

   - colorScheme drives tldraw's own dark/light theme via user prefs.
   - bg recolors the canvas background (CSS custom property on the
     board container) so the board blends with the Emacs frame.
   - palette (optional) overrides tldraw's named shape colors to match
     the theme's faces.  */

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

  /* Recolor the canvas background + palette via CSS variables on the
     board's container.  tldraw reads several --color-* vars; the
     container is tagged with data-board so theme.css can scope.  */
  const container = editor.getContainer?.();
  if (container) {
    if (msg.bg) {
      container.style.setProperty('--slop-canvas-bg', msg.bg);
    }
    if (msg.palette && typeof msg.palette === 'object') {
      for (const [name, hex] of Object.entries(msg.palette)) {
        if (typeof hex === 'string') {
          container.style.setProperty(`--slop-color-${name}`, hex);
        }
      }
    }
  }
}
