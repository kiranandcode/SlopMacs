/* Overlays tldraw boards on the windows that show `.tldr' buffers.

   Mirrors WebviewLayer (App.jsx): a window whose buffer sets
   `web-tldraw-board-id' streams a non-empty `tldraw' field; this layer
   positions a <TldrawBoard> over that window's body using the window's
   Emacs pixel geometry (px/py/pw/ph), which maps 1:1 onto CSS pixels in
   .frame-container.

   Boards are heavy (each has its own store + canvas), so they are
   pooled by board-id and merely hidden (display:none) when no window
   shows them — switching Emacs tabs/buffers away and back must not
   remount (which would reload from Emacs and flash).  Past the cap the
   least-recently-visible board is evicted; since the content lives in
   Emacs, an evicted board simply reloads when it reappears.  */

import React, { useEffect, useRef, useState } from 'react';
import TldrawBoard from './TldrawBoard.jsx';

const MAX_POOLED_BOARDS = 4;

export default function TldrawLayer ({ state, getWs }) {
  const [snap, setSnap] = useState({ views: [], pool: [] });
  const poolRef = useRef(new Map());   /* board-id -> LRU stamp */
  const stampRef = useRef(0);

  useEffect(() => {
    const recompute = () => {
      const views = [];
      for (const w of state.windows.values()) {
        if (w.tldraw) {
          views.push({
            id: w.id,
            board: w.tldraw,
            x: w.px || 0,
            y: w.py || 0,
            w: w.pw || 0,
            h: w.ph || 0,
          });
        }
      }
      views.sort((a, b) => a.id - b.id);

      const pool = poolRef.current;
      const stamp = ++stampRef.current;
      for (const v of views) pool.set(v.board, stamp);
      if (pool.size > MAX_POOLED_BOARDS) {
        const visible = new Set(views.map(v => v.board));
        const evictable = [...pool.entries()]
          .filter(([id]) => !visible.has(id))
          .sort((a, b) => a[1] - b[1]);
        for (const [id] of evictable) {
          if (pool.size <= MAX_POOLED_BOARDS) break;
          pool.delete(id);
        }
      }

      const next = { views, pool: [...pool.keys()].sort() };
      setSnap(prev =>
        JSON.stringify(prev) === JSON.stringify(next) ? prev : next);
    };
    recompute();
    return state.onChange(recompute);
  }, [state]);

  if (snap.pool.length === 0) return null;
  return (
    <div className="tldraw-layer">
      {snap.pool.map(board => {
        const v = snap.views.find(x => x.board === board);
        return (
          <div
            key={board}
            className="tldraw-board"
            style={v
              ? { left: v.x, top: v.y, width: v.w, height: v.h }
              : { display: 'none' }}
          >
            <TldrawBoard boardId={board} state={state} getWs={getWs} />
          </div>
        );
      })}
    </div>
  );
}
