/* A single tldraw whiteboard, the canvas view of an Emacs `.tldr'
   buffer.  The board's authoritative content lives in Emacs; this
   component loads snapshots it is handed (`tldraw_load') and pushes
   debounced snapshots back (`tldraw_snapshot') as the document changes.
   An epoch counter prevents echo loops: a snapshot is stamped with the
   epoch of the load it descends from, and Emacs ignores snapshots it
   would merely be echoing.

   Persistence is driven two ways: a store listener catches direct mouse
   edits, and every applied `tldraw_cmd' explicitly schedules a push —
   the latter is necessary because programmatic store changes do not
   reliably flush tldraw's document-scope listeners.  Both funnel
   through the same debounce.  */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Tldraw } from 'tldraw';
import 'tldraw/tldraw.css';
import { applyCommand } from './navigation.js';
import { applyTheme } from './theme.js';

const SNAPSHOT_DEBOUNCE_MS = 400;

/* Module-level (referentially stable) so it is NOT a fresh function on
   every render — an inline getShapeVisibility makes tldraw re-create the
   editor each render, which shows an endless loading spinner and churns
   CPU.  Hides folded group members (meta.slopHidden).  */
const SHAPE_VISIBILITY = (shape) =>
  (shape.meta && shape.meta.slopHidden) ? 'hidden' : 'inherit';

export default function TldrawBoard ({ boardId, state, getWs }) {
  const [editor, setEditor] = useState(null);
  const editorRef = useRef(null);
  const loadedEpochRef = useRef(0);
  const applyingRemoteRef = useRef(false);
  const snapTimerRef = useRef(null);

  /* Keep getWs current without making `send' churn (and re-run the
     effects that register listeners) on every App render.  */
  const getWsRef = useRef(getWs);
  getWsRef.current = getWs;
  const send = useCallback((obj) => {
    const ws = getWsRef.current && getWsRef.current();
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj) + '\n');
    }
  }, []);

  const pushSnapshot = useCallback(() => {
    const ed = editorRef.current;
    if (!ed) return;
    let snap;
    try { snap = ed.getSnapshot(); }
    catch (e) { console.error('tldraw getSnapshot failed:', e); return; }
    send({ type: 'tldraw_snapshot', board: boardId,
           epoch: loadedEpochRef.current, snapshot: snap });
  }, [send, boardId]);

  const scheduleSnapshot = useCallback(() => {
    if (applyingRemoteRef.current) return;
    if (snapTimerRef.current) clearTimeout(snapTimerRef.current);
    snapTimerRef.current = setTimeout(pushSnapshot, SNAPSHOT_DEBOUNCE_MS);
  }, [pushSnapshot]);

  const onMount = useCallback((ed) => {
    editorRef.current = ed;
    (window._tldrawBoards = window._tldrawBoards || {})[boardId] = ed;
    setEditor(ed);
  }, [boardId]);

  /* Initial theme + a store listener for direct mouse edits + readiness
     announce (so Emacs sends the first snapshot).  Tied to the editor's
     identity so it survives App re-renders.  */
  useEffect(() => {
    if (!editor) return undefined;
    applyTheme(editor, { colorScheme: state.colorScheme });
    const unlisten = editor.store.listen(() => scheduleSnapshot(),
                                          { scope: 'document' });
    send({ type: 'tldraw_event', board: boardId, event: 'ready' });
    return () => {
      unlisten();
      if (snapTimerRef.current) clearTimeout(snapTimerRef.current);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [editor, boardId, send, scheduleSnapshot]);

  /* Inbound control channel (Emacs -> this board).  */
  useEffect(() => {
    if (!editor) return undefined;
    const off = state.onBoardMessage((msg) => {
      const targeted = msg.board === boardId;
      const broadcast = !msg.board && msg.type === 'tldraw_theme';
      if (!targeted && !broadcast) return;

      switch (msg.type) {
        case 'tldraw_load':
          loadedEpochRef.current = msg.epoch || 0;
          applyingRemoteRef.current = true;
          try {
            if (msg.snapshot) editor.loadSnapshot(msg.snapshot);
          } catch (e) {
            console.error('tldraw_load failed:', e);
          } finally {
            applyingRemoteRef.current = false;
          }
          break;
        case 'tldraw_cmd':
          applyCommand(editor, msg.verb, msg.args || {}, { send, boardId });
          /* Programmatic edits don't reliably flush the store listener,
             so persist explicitly (debounced).  */
          scheduleSnapshot();
          break;
        case 'tldraw_node_text':
          applyCommand(editor, 'node-text', msg, { send, boardId });
          scheduleSnapshot();
          break;
        case 'tldraw_theme':
          applyTheme(editor, msg);
          break;
        case 'tldraw_config': {
          const ui = msg.ui || {};
          /* tldraw's hideUi prop does not react after mount, so hide the
             UI layer via CSS on the board container instead.  */
          if (typeof ui.hideUi === 'boolean') {
            const c = editor.getContainer?.();
            const board = c && c.closest('.tldraw-board');
            if (board) board.classList.toggle('hide-ui', ui.hideUi);
          }
          if (typeof ui.grid === 'boolean') {
            try { editor.updateInstanceState({ isGridMode: ui.grid }); }
            catch (e) { /* ignore */ }
          }
          break;
        }
        case 'tldraw_layout':
          import('./spytial.js')
            .then(m => m.applyLayout(editor, msg, { send, boardId }))
            .then(() => scheduleSnapshot())
            .catch(e => console.error('spytial layout failed:', e));
          break;
        default:
          break;
      }
    });
    return off;
  }, [editor, boardId, state, send, scheduleSnapshot]);

  /* autoFocus=false is critical: by default tldraw grabs document focus
     on mount and its global keyboard handlers then swallow keystrokes
     before they reach Emacs (pointer-events:none stops the mouse, not
     the keyboard).  Keyboard-first means Emacs must keep focus; the
     board is driven through the Editor API.  When mouse-mode is on the
     user clicks into the board to interact with it directly.  */
  return (
    <Tldraw
      onMount={onMount}
      autoFocus={false}
      getShapeVisibility={SHAPE_VISIBILITY}
    />
  );
}
