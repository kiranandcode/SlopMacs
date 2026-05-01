/* Performance measurement - injected via import.
   Measures dispatch-to-paint time for frame_update messages.  */

export function instrumentPerf (state) {
  window._cycleLog = [];
  window._dispatchLog = [];

  const origDispatch = state.dispatch.bind(state);

  state.dispatch = function (msg) {
    if (msg.type !== 'frame_update') {
      return origDispatch(msg);
    }

    const t0 = performance.now();
    origDispatch(msg);
    const dispatchTime = performance.now() - t0;

    window._dispatchLog.push(+dispatchTime.toFixed(2));
    if (window._dispatchLog.length > 50) window._dispatchLog.shift();

    requestAnimationFrame(() => {
      const paintTime = performance.now() - t0;
      window._cycleLog.push(+paintTime.toFixed(1));
      if (window._cycleLog.length > 50) window._cycleLog.shift();
    });
  };
}
