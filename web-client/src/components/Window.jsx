/* Window component — renders all lines for a single Emacs window.
   Uses React.memo to skip re-rendering if the window hasn't changed.  */

import React from 'react';
import Line from './Line.jsx';

function Window ({ win, faceResolver, gen, cursorGen }) {
  const lines = new Array(win.h);

  for (let row = 0; row < win.h; row++) {
    const lineData = win.lines.get(row);
    lines[row] = (
      <Line
        key={row}
        lineData={lineData}
        lineGen={lineData ? lineData.gen : 0}
        cursor={win.cursor}
        cursorGen={cursorGen}
        faceResolver={faceResolver}
      />
    );
  }

  const style = {
    left: 'calc(' + win.x + ' * var(--char-w))',
    top: 'calc(' + win.y + ' * var(--char-h))',
    width: 'calc(' + win.w + ' * var(--char-w))',
    height: 'calc(' + win.h + ' * var(--char-h))',
  };

  return <div className="emacs-window" style={style}>{lines}</div>;
}

export default React.memo(Window, (prev, next) => {
  return prev.gen === next.gen
      && prev.cursorGen === next.cursorGen;
});
