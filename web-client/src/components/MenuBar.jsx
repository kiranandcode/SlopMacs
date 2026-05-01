/* Menu bar component — renders Emacs menu bar as a GUI menu bar.
   Extracts menu item names from the text line and sends mouse events
   on click to trigger Emacs popup menus.  */

import React, { useCallback, useRef } from 'react';
import { measureFont } from '../measure.js';

function MenuBar ({ win, gen }) {
  const lineData = win.lines.get(0);
  const metricsRef = useRef(null);

  const handleClick = useCallback((item) => {
    const ws = window._ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    if (!metricsRef.current) metricsRef.current = measureFont();
    const m = metricsRef.current;

    const x = (item.col + 1) * m.charWInt;
    const y = win.y * m.charHInt + Math.floor(m.charHInt / 2);

    ws.send(JSON.stringify({ type: 'mouse_down', x, y, button: 0, mods: 0 }) + '\n');
    ws.send(JSON.stringify({ type: 'mouse_up', x, y, button: 0, mods: 0 }) + '\n');
  }, [win.y]);

  if (!lineData || !lineData.runs) {
    return <div className="menu-bar" />;
  }

  let fullText = '';
  for (const run of lineData.runs) {
    fullText += run.text || '';
  }

  const items = [];
  let i = 0;
  while (i < fullText.length) {
    if (fullText[i] === ' ') { i++; continue; }
    const start = i;
    while (i < fullText.length && fullText[i] !== ' ') i++;
    items.push({ label: fullText.substring(start, i), col: start });
  }

  const style = {
    left: `calc(${win.x} * var(--char-w))`,
    top: `calc(${win.y} * var(--char-h))`,
    width: `calc(${win.w} * var(--char-w))`,
    height: `calc(${win.h} * var(--char-h))`,
  };

  return (
    <div className="menu-bar" style={style}>
      {items.map((item) => (
        <div
          key={item.col}
          className="menubar-item"
          onClick={() => handleClick(item)}
        >
          {item.label}
        </div>
      ))}
    </div>
  );
}

export default React.memo(MenuBar, (prev, next) => {
  return prev.gen === next.gen;
});
