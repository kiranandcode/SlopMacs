/* Frame component — owns the imperative editor surface.
   React mounts the surface once; renderEngine handles hot-path updates.  */

import React, { useEffect, useRef } from 'react';
import { measureFont } from '../measure.js';
import { FrameRenderer } from '../renderEngine.js';

function Frame ({ state }) {
  const rootRef = useRef(null);
  const rendererRef = useRef(null);

  useEffect(() => {
    if (!rootRef.current) return undefined;

    const renderer = new FrameRenderer(rootRef.current, measureFont());
    rendererRef.current = renderer;
    window._renderer = renderer;   /* debug REPL access */
    renderer.schedule(state);

    const unsubscribe = state.onChange((immediate) => {
      if (immediate)
        renderer.renderNow(state);
      else
        renderer.schedule(state);
    });
    return () => {
      unsubscribe();
      renderer.destroy();
      rendererRef.current = null;
    };
  }, [state]);

  return <div ref={rootRef} className="emacs-frame" />;
}

export default React.memo(Frame);
