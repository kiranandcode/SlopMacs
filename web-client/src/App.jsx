/* Emacs web display — React application root.
   Manages WebSocket connection, state updates, and rendering.  */

import React, { useState, useEffect, useCallback, useRef } from 'react';
import { FrameState } from './state.js';
import { InputHandler } from './input.js';
import { measureFont } from './measure.js';
import Frame from './components/Frame.jsx';
import Menu from './components/Menu.jsx';
import TldrawLayer from './tldraw/TldrawLayer.jsx';
import './widgets.js';

const state = new FrameState();
window._state = state;

/* Iframes overlaid on windows whose buffer sets `web-webview-url'
   (Emacs side: web-webview.el / webterm.c).  Subscribes to FrameState
   directly — App deliberately does not re-render on frame updates.
   Geometry is Emacs frame pixels, which map 1:1 onto CSS pixels in
   .frame-container.

   Iframes are pooled by URL and merely hidden when no visible window
   shows them (display:none keeps the page alive, like a background
   browser tab) — switching Emacs tabs or buffers away and back must
   not reload the embedded app.  Keys are URLs, so React never
   remounts a pooled iframe while it stays in the pool; the
   least-recently-visible entries are evicted past the cap.  */
const MAX_POOLED_WEBVIEWS = 6;

function WebviewLayer ({ state }) {
  const [snap, setSnap] = useState({ views: [], pool: [] });
  const poolRef = useRef(new Map());   /* url -> LRU stamp */
  const stampRef = useRef(0);

  useEffect(() => {
    const recompute = () => {
      const views = [];
      for (const w of state.windows.values()) {
        if (w.webview) {
          views.push({
            id: w.id,
            url: w.webview,
            x: w.px || 0,
            y: w.py || 0,
            w: w.pw || 0,
            h: w.webviewPh || w.ph || 0,
          });
        }
      }
      views.sort((a, b) => a.id - b.id);

      const pool = poolRef.current;
      const stamp = ++stampRef.current;
      for (const v of views) pool.set(v.url, stamp);
      if (pool.size > MAX_POOLED_WEBVIEWS) {
        const visible = new Set(views.map(v => v.url));
        const evictable = [...pool.entries()]
          .filter(([url]) => !visible.has(url))
          .sort((a, b) => a[1] - b[1]);
        for (const [url] of evictable) {
          if (pool.size <= MAX_POOLED_WEBVIEWS) break;
          pool.delete(url);
        }
      }

      const next = { views, pool: [...pool.keys()].sort() };
      setSnap(prev =>
        JSON.stringify(prev) === JSON.stringify(next) ? prev : next);
    };
    recompute();          /* current state, not just future changes */
    return state.onChange(recompute);
  }, [state]);

  if (snap.pool.length === 0) return null;
  return (
    <div className="webview-layer">
      {snap.pool.map(url => {
        const here = snap.views.filter(v => v.url === url);
        const v = here[0];
        return (
          <React.Fragment key={url}>
            <iframe
              className="webview-frame"
              src={url}
              style={v
                ? { left: v.x, top: v.y, width: v.w, height: v.h }
                : { display: 'none' }}
            />
            {/* Rare: the same URL visible in several windows at once.
                Extra copies are ephemeral (they reload on remount).  */}
            {here.slice(1).map((dv, i) => (
              <iframe
                key={`${url}#${i}`}
                className="webview-frame"
                src={url}
                style={{ left: dv.x, top: dv.y, width: dv.w, height: dv.h }}
              />
            ))}
          </React.Fragment>
        );
      })}
    </div>
  );
}

export default function App () {
  const [, forceUpdate] = useState(0);
  const frameRef = useRef(null);
  const wsRef = useRef(null);
  const inputRef = useRef(null);
  const busyTimerRef = useRef(null);
  const busyRef = useRef(false);
  const msgCountRef = useRef(0);
  const msgCountElRef = useRef(null);
  const activeMenuRef = useRef(null);
  const hasFirstFrameRef = useRef(false);
  const [connStatus, setConnStatus] = useState('disconnected');
  const [busy, setBusy] = useState(false);

  /* The pill means "Emacs looks wedged": no traffic at all for two
     heartbeat intervals (WEB_HEARTBEAT_MS = 5000 on the C side).  Any
     message resets the timer — with the preemptive executor, redisplay
     keeps streaming even while a command grinds, so silence is the
     only meaningful distress signal.  */
  const resetHeartbeat = useCallback(() => {
    if (busyTimerRef.current) clearTimeout(busyTimerRef.current);
    busyTimerRef.current = setTimeout(() => {
      busyRef.current = true;
      setBusy(true);
    }, 11000);
    if (busyRef.current) {
      busyRef.current = false;
      setBusy(false);
    }
  }, []);

  /* React only tracks chrome state.  The frame renderer listens
     directly to FrameState and bypasses React for editor paints.  */
  useEffect(() => {
    return state.onChange(() => {
      if (activeMenuRef.current !== state.activeMenu
          || hasFirstFrameRef.current !== state.hasFirstFrame) {
        activeMenuRef.current = state.activeMenu;
        hasFirstFrameRef.current = state.hasFirstFrame;
        forceUpdate(c => c + 1);
      }
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

        const metrics = measureFont();
        ws.send(JSON.stringify({
          type: 'font_metrics',
          char_w: metrics.charWInt,
          char_h: metrics.charHInt,
          asc: metrics.ascent,
          desc: metrics.descent,
        }) + '\n');

        if (frameRef.current) {
          const rect = frameRef.current.getBoundingClientRect();
          const cols = Math.floor(rect.width / metrics.charW);
          const rows = Math.floor(rect.height / metrics.charH);
          if (cols > 0 && rows > 0) {
            ws.send(JSON.stringify({ type: 'resize', cols, rows }) + '\n');
          }
        }

        if (document.hasFocus()) {
          ws.send(JSON.stringify({ type: 'focus', gained: true }) + '\n');
        }

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
            return;
          } else if (msg.type === 'eval') {
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
          state.dispatch(msg);
          msgCountRef.current++;
          if (msgCountElRef.current) {
            msgCountElRef.current.textContent = msgCountRef.current + ' msgs';
          }
        } catch (e) {
          /* Ignore parse errors.  */
        }
      };

      ws.onclose = () => {
        setConnStatus('disconnected');
        busyRef.current = false;
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

  const dismissMenu = useCallback(() => {
    state.dismissMenu();
  }, []);

  const [loadingDone, setLoadingDone] = useState(false);
  useEffect(() => {
    if (connStatus === 'connected' && state.hasFirstFrame && !loadingDone) {
      const t = setTimeout(() => setLoadingDone(true), 2000);
      return () => clearTimeout(t);
    }
  }, [connStatus, state.hasFirstFrame, loadingDone]);
  const showLoading = !loadingDone;

  return (
    <div className="app-root">
      {/* Loading screen with fade-out transition.  */}
      {showLoading && (
        <div className={'loading-screen' + (connStatus === 'connected' && state.hasFirstFrame ? ' fading' : '')}>
          <div className="loading-content">
            <img src="./emacs-slop.svg" alt="" className="loading-icon" />
            <div className="loading-logo">Slopmacs</div>
            <div className="loading-bar"><div className="loading-bar-fill" /></div>
            <div className="loading-status">
              {connStatus === 'disconnected' ? 'Waiting for Emacs...'
                : connStatus === 'connecting' ? 'Connecting...'
                : 'Loading display...'}
            </div>
          </div>
        </div>
      )}

      {/* Main frame area.  */}
      <div
        ref={frameRef}
        className="frame-container"
        tabIndex={0}
      >
        <Frame state={state} />
        <WebviewLayer state={state} />
        <TldrawLayer state={state} getWs={() => wsRef.current} />
      </div>

      {state.activeMenu && wsRef.current && (
        <Menu menu={state.activeMenu} ws={wsRef.current} onDismiss={dismissMenu} />
      )}

      {busy && (
        <div className="busy-pill">
          Emacs is busy...
        </div>
      )}

      <div className="status-bar">
        <span className={'status-dot ' + connStatus} />
        <span className="status-text">
          {connStatus === 'connected' ? 'Connected'
            : connStatus === 'connecting' ? 'Connecting...'
            : 'Disconnected'}
        </span>
        <span className="status-right">
          {connStatus === 'connected' && (
            <button className="interrupt-btn" onClick={interruptClick}>
              Interrupt
            </button>
          )}
          <span ref={msgCountElRef} className="msg-count">0 msgs</span>
        </span>
      </div>
    </div>
  );
}
