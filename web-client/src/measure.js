/* Measure monospace font metrics.
   Uses the same styles as the rendering context (monospace 16px, line-height 1.2)
   so the measured values match what the browser actually renders.  */

let cached = null;

export function measureFont () {
  if (cached) return cached;

  const el = document.createElement('div');
  el.style.fontFamily = 'monospace';
  el.style.fontSize = '16px';
  el.style.lineHeight = '1.2';
  el.style.position = 'absolute';
  el.style.visibility = 'hidden';
  el.style.whiteSpace = 'pre';
  /* Measure 10 chars and divide for sub-pixel precision.  */
  el.textContent = 'MMMMMMMMMM';
  document.body.appendChild(el);

  const rect = el.getBoundingClientRect();
  const charW = rect.width / 10;
  const charH = rect.height;  /* already includes line-height: 1.2 */
  document.body.removeChild(el);

  const charWInt = Math.round(charW);
  const charHInt = Math.round(charH);

  cached = {
    charW,       /* fractional — for browser pixel positioning */
    charH,       /* fractional — for browser pixel positioning */
    charWInt,    /* integer — sent to Emacs in font_metrics */
    charHInt,    /* integer — sent to Emacs in font_metrics */
    ascent: Math.round(charHInt * 2 / 3),
    descent: charHInt - Math.round(charHInt * 2 / 3),
  };

  return cached;
}

export function invalidateMetrics () {
  cached = null;
}
