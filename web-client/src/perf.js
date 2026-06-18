/* Lightweight always-on performance log.

   Surfaced on `window.__perf` and `window.__perfSummary()` so it can be
   inspected live from the debug REPL (port+1) while reproducing a
   problem — e.g. press C-v, then run __perfSummary() to see how many
   frames Emacs sent, how long each canvas render took, and the FX
   overlay tick cost.  Recording is a single array push per frame, so it
   stays effectively free.  */

const CAP = 600;
const perf = { render: [], fxtick: [], msg: [], t0: performance.now() };
window.__perf = perf;

function push (arr, v) {
  arr.push(v);
  if (arr.length > CAP) arr.shift();
}

export function recordRender (ms) { push(perf.render, +ms.toFixed(2)); }
export function recordFxTick (ms) { push(perf.fxtick, +ms.toFixed(2)); }
export function recordFrameMsg () { push(perf.msg, performance.now()); }

window.__perfReset = function () {
  perf.render.length = 0;
  perf.fxtick.length = 0;
  perf.msg.length = 0;
  perf.t0 = performance.now();
  return 'reset';
};

function stat (a) {
  if (!a.length) return null;
  const s = a.slice().sort((x, y) => x - y);
  const sum = a.reduce((x, y) => x + y, 0);
  return {
    n: a.length,
    avg: +(sum / a.length).toFixed(2),
    p50: s[Math.floor(a.length / 2)],
    max: s[s.length - 1],
  };
}

window.__perfSummary = function () {
  const el = Math.max(0.001, (performance.now() - perf.t0) / 1000);
  return JSON.stringify({
    elapsed_s: +el.toFixed(2),
    /* Canvas text render: wall time per FrameRenderer.render().  */
    render: stat(perf.render),
    renders_per_s: +(perf.render.length / el).toFixed(1),
    /* FX overlay (trail/beacon/flash) per-frame cost.  */
    fxtick: stat(perf.fxtick),
    fxticks_per_s: +(perf.fxtick.length / el).toFixed(1),
    /* frame_update messages received from Emacs.  A flood here (≫60/s)
       means the server is over-sending (e.g. a too-fast scroll loop).  */
    frames_from_emacs: perf.msg.length,
    frames_per_s: +(perf.msg.length / el).toFixed(1),
  });
};
