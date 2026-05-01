/* Emacs web display — Preact application root.
   Manages WebSocket connection, state updates, and rendering.  */

import { h, render } from 'preact';
import { useState, useEffect, useCallback, useRef } from 'preact/hooks';
import { FrameState } from './state.js';
import { renderFrame } from './components/frame.js';
import { InputHandler } from './input.js';
import { renderMenu } from './components/menu.js';

const state = new FrameState();
window._state = state;  /* expose for debug REPL */

function App () {
  const [, forceUpdate] = useState(0);
  const frameRef = useRef(null);
  const wsRef = useRef(null);
  const inputRef = useRef(null);
  const busyTimerRef = useRef(null);
  const [connStatus, setConnStatus] = useState('disconnected');
  const [msgCount, setMsgCount] = useState(0);
  const [busy, setBusy] = useState(false);

  const resetHeartbeat = useCallback(() => {
    setBusy(false);
    if (busyTimerRef.current) clearTimeout(busyTimerRef.current);
    busyTimerRef.current = setTimeout(() => setBusy(true), 1000);
  }, []);

  /* Schedule re-render on state changes, batched via rAF.  */
  useEffect(() => {
    let rafId = null;
    state.onChange(() => {
      if (rafId) return;
      rafId = requestAnimationFrame(() => {
        rafId = null;
        forceUpdate(c => c + 1);
      });
    });
  }, []);

  /* WebSocket connection.  */
  useEffect(() => {
    let reconnectDelay = 1000;
    let destroyed = false;

    function connect () {
      if (destroyed) return;
      const wsPort = new URLSearchParams(location.search).get('port') || 8080;
      const wsUrl = `ws://${location.hostname || 'localhost'}:${wsPort}`;
      setConnStatus('connecting');

      const ws = new WebSocket(wsUrl);
      wsRef.current = ws;

      ws.onopen = () => {
        setConnStatus('connected');
        resetHeartbeat();
        reconnectDelay = 1000;

        /* Send font metrics.  */
        const metrics = measureFont();
        ws.send(JSON.stringify({
          type: 'font_metrics',
          char_w: metrics.charW,
          char_h: metrics.charH,
          asc: metrics.ascent,
          desc: metrics.descent,
        }) + '\n');

        /* Send initial resize.  */
        if (frameRef.current) {
          const rect = frameRef.current.getBoundingClientRect();
          const cols = Math.floor(rect.width / metrics.charW);
          const rows = Math.floor(rect.height / metrics.charH);
          if (cols > 0 && rows > 0) {
            ws.send(JSON.stringify({ type: 'resize', cols, rows }) + '\n');
          }
        }

        /* Send focus state.  */
        if (document.hasFocus()) {
          ws.send(JSON.stringify({ type: 'focus', gained: true }) + '\n');
        }

        /* Set up input handler.  */
        if (inputRef.current) inputRef.current.destroy();
        inputRef.current = new InputHandler(frameRef.current, ws);
        window._input = inputRef.current;
        window._ws = ws;
      };

      ws.onmessage = (event) => {
        if (typeof event.data !== 'string') return;

        try {
          const msg = JSON.parse(event.data);
          if (msg.type === 'heartbeat') {
            resetHeartbeat();
            return; /* Don't dispatch or count heartbeats.  */
          } else if (msg.type === 'eval') {
            /* Debug REPL: evaluate JS and send result back.  */
            let result;
            try {
              result = (0, eval)(msg.code);
              ws.send(JSON.stringify({
                type: 'eval_result',
                result: typeof result === 'undefined' ? 'undefined'
                  : JSON.stringify(result, null, 2)
              }) + '\n');
            } catch (evalErr) {
              ws.send(JSON.stringify({
                type: 'eval_result',
                error: evalErr.message
              }) + '\n');
            }
            return;
          }
          if (msg.ts) {
            const now = Date.now();
            const delta = now - msg.ts;
            if (delta > 50) console.warn(`[latency] ${delta}ms (emacs→browser)`);
          }
          state.dispatch(msg);
          setMsgCount(c => c + 1);
        } catch (e) {
          /* Ignore parse errors.  */
        }
      };

      ws.onclose = () => {
        setConnStatus('disconnected');
        setBusy(false);
        if (busyTimerRef.current) {
          clearTimeout(busyTimerRef.current);
          busyTimerRef.current = null;
        }
        if (inputRef.current) {
          inputRef.current.destroy();
          inputRef.current = null;
        }
        if (!destroyed) {
          setTimeout(() => {
            reconnectDelay = Math.min(reconnectDelay * 1.5, 10000);
            connect();
          }, reconnectDelay);
        }
      };

      ws.onerror = () => {
        setConnStatus('disconnected');
      };
    }

    connect();
    return () => {
      destroyed = true;
      if (wsRef.current) wsRef.current.close();
      if (inputRef.current) inputRef.current.destroy();
    };
  }, [resetHeartbeat]);

  /* Handle window resize.  */
  useEffect(() => {
    if (!frameRef.current) return;
    const observer = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const metrics = measureFont();
        const cols = Math.floor(entry.contentRect.width / metrics.charW);
        const rows = Math.floor(entry.contentRect.height / metrics.charH);
        if (cols > 0 && rows > 0 && wsRef.current
            && wsRef.current.readyState === WebSocket.OPEN) {
          wsRef.current.send(
            JSON.stringify({ type: 'resize', cols, rows }) + '\n'
          );
        }
      }
    });
    observer.observe(frameRef.current);
    return () => observer.disconnect();
  }, []);

  const interruptClick = () => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ type: 'interrupt' }) + '\n');
    }
  };

  return h('div', { style: { width: '100%', height: '100%' } },
    h('div', { ref: frameRef, tabIndex: 0, style: { width: '100%', height: 'calc(100% - 24px)', outline: 'none' } },
      renderFrame(state, h, wsRef.current)
    ),
    state.activeMenu && wsRef.current && renderMenu(state.activeMenu, wsRef.current, state, h),
    busy && h('div', { id: 'busy-overlay', style: { display: 'block' } }, 'Emacs is busy...'),
    h('div', { id: 'status-bar' },
      h('span', { class: connStatus }, connStatus === 'connected' ? 'Connected' : connStatus === 'connecting' ? 'Connecting...' : 'Disconnected'),
      h('span', null,
        h('button', { id: 'interrupt-btn', style: { display: connStatus === 'connected' ? 'inline-block' : 'none' }, onClick: interruptClick }, 'Interrupt'),
        ' ',
        h('span', null, `${msgCount} msgs`)
      )
    )
  );
}

/* Measure monospace font metrics.  */
function measureFont () {
  const el = document.createElement('span');
  el.style.fontFamily = 'monospace';
  el.style.fontSize = '16px';
  el.style.position = 'absolute';
  el.style.visibility = 'hidden';
  el.style.whiteSpace = 'pre';
  el.textContent = 'M';
  document.body.appendChild(el);

  const rect = el.getBoundingClientRect();
  const charW = Math.ceil(rect.width);
  const charH = Math.ceil(rect.height * 1.2); /* match line-height: 1.2 */
  const ascent = Math.ceil(rect.height * 0.8);
  const descent = charH - ascent;
  document.body.removeChild(el);

  return { charW, charH, ascent, descent };
}

render(h(App), document.getElementById('app'));
