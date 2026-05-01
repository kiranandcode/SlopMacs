/* Frame component — renders all Emacs windows.  */

import { renderWindow } from './window.js';
import { renderMenuBar } from './menubar.js';

export function renderFrame (state, h, ws) {
  const faceResolver = (faceId) => state.getFace(faceId);

  const windows = [];
  /* Sort windows by position (top-left first).  */
  const sorted = [...state.windows.values()].sort((a, b) => {
    if (a.y !== b.y) return a.y - b.y;
    return a.x - b.x;
  });

  for (const win of sorted) {
    if (win.menuBar) {
      windows.push(renderMenuBar(win, ws, h));
    } else {
      windows.push(renderWindow(win, faceResolver, h));
    }
  }

  const style = {
    backgroundColor: state.defaultBg,
    color: state.defaultFg,
  };

  return h('div', { class: 'emacs-frame', style }, ...windows);
}
