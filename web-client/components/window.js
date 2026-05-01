/* Window component — renders all lines for a single Emacs window.  */

import { renderLine } from './line.js';

export function renderWindow (win, faceResolver, h) {
  const lines = [];

  for (let row = 0; row < win.h; row++) {
    const lineData = win.lines.get(row);
    /* Use a key combining window id + row for stable identity.  */
    lines.push(renderLine(lineData, win.cursor, faceResolver, h, row));
  }

  const style = {
    left: `calc(${win.x} * 1ch)`,
    top: `calc(${win.y} * 1.2em)`,
    width: `calc(${win.w} * 1ch)`,
    height: `calc(${win.h} * 1.2em)`,
  };

  return h('div', { class: 'emacs-window', key: 'w' + win.id, style }, ...lines);
}
