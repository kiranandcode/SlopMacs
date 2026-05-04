/* Measure monospace font metrics.
   These values must match the canvas renderer and CSS frame.  */

let cached = null;

export const FONT_FAMILY = "'Menlo','SF Mono','Cascadia Code','Fira Code',"
  + "'JetBrains Mono','Consolas','Liberation Mono',monospace";
export const FONT_SIZE = 16;
export const LINE_HEIGHT = 1.2;

export function measureFont () {
  if (cached) return cached;

  const el = document.createElement('div');
  el.style.fontFamily = FONT_FAMILY;
  el.style.fontSize = FONT_SIZE + 'px';
  el.style.lineHeight = String(LINE_HEIGHT);
  el.style.position = 'absolute';
  el.style.visibility = 'hidden';
  el.style.whiteSpace = 'pre';
  /* Measure 10 chars and divide for sub-pixel precision.  */
  el.textContent = 'MMMMMMMMMM';
  document.body.appendChild(el);

  const rect = el.getBoundingClientRect();
  const rawCharW = rect.width / 10;
  const rawCharH = rect.height;  /* already includes line-height: 1.2 */
  document.body.removeChild(el);

  const charWInt = Math.round(rawCharW);
  const charHInt = Math.round(rawCharH);

  cached = {
    charW: charWInt,
    charH: charHInt,
    rawCharW,
    rawCharH,
    charWInt,
    charHInt,
    ascent: Math.round(charHInt * 2 / 3),
    descent: charHInt - Math.round(charHInt * 2 / 3),
  };

  return cached;
}

export function invalidateMetrics () {
  cached = null;
}
