/* Web applet overlay system for Emacs web display.
   Widgets are interactive HTML applets that live inside Emacs buffers.
   They piggyback on the IMAGE_GLYPH pipeline: a transparent SVG
   placeholder reserves space, and the browser renders interactive
   content over it instead of drawing the static image.  */

import { measureFont } from './measure.js';

const overlays = new Map();
const loadedScripts = new Map();

/* Widget registry: widget-id → { el, rendered, renderFn } */
const widgetRegistry = new Map();

function getFrameRoot () {
  return document.querySelector('.emacs-frame');
}

/* ------------------------------------------------------------------ */
/*  Generic overlay management                                         */
/* ------------------------------------------------------------------ */

function createOverlay (id, opts = {}) {
  let el = overlays.get(id);
  if (el) {
    el.innerHTML = '';
    return el;
  }
  const root = getFrameRoot();
  if (!root) return null;
  el = document.createElement('div');
  el.id = 'widget-overlay-' + id;
  el.className = 'widget-overlay';
  el.style.cssText =
    'position:absolute;z-index:10;pointer-events:auto;overflow:hidden;';
  if (opts.fullFrame) {
    el.style.inset = '0';
  } else {
    el.style.left = (opts.x || 0) + 'px';
    el.style.top = (opts.y || 0) + 'px';
    el.style.width = (opts.width || 400) + 'px';
    el.style.height = (opts.height || 300) + 'px';
  }
  root.appendChild(el);
  overlays.set(id, el);
  return el;
}

function destroyOverlay (id) {
  const el = overlays.get(id);
  if (el) { el.remove(); overlays.delete(id); }
  widgetRegistry.delete(id);
}

function destroyAllOverlays () {
  for (const [, el] of overlays) el.remove();
  overlays.clear();
  widgetRegistry.clear();
}

/* ------------------------------------------------------------------ */
/*  Widget registry (buffer-integrated via IMAGE_GLYPH pipeline)       */
/* ------------------------------------------------------------------ */

/* Register a widget.  renderFn(el) is called once when the widget
   first becomes visible.  The overlay is then repositioned on every
   frame update as the image glyph moves with buffer scroll.  */
function registerWidget (id, renderFn) {
  let entry = widgetRegistry.get(id);
  if (entry) {
    /* Re-register: destroy old content.  */
    if (entry.el) entry.el.remove();
    overlays.delete(id);
  }
  widgetRegistry.set(id, { el: null, rendered: false, renderFn });
}

/* Called by renderEngine.drawImages() when it finds a widget image.
   Positions the overlay at the image's exact pixel coordinates.
   Optional clip insets (top, right, bottom, left) restrict visibility
   to the window content area without shrinking the element (so that
   offsetWidth/offsetHeight remain correct for rendering code).  */
function positionWidget (id, x, y, w, h, clipT, clipR, clipB, clipL) {
  const entry = widgetRegistry.get(id);
  if (!entry) return;

  if (!entry.el) {
    const root = getFrameRoot();
    if (!root) return;
    const el = document.createElement('div');
    el.id = 'widget-' + id;
    el.className = 'widget-overlay';
    el.style.cssText =
      'position:absolute;z-index:10;pointer-events:auto;overflow:hidden;';
    root.appendChild(el);
    overlays.set(id, el);
    entry.el = el;
  }

  const el = entry.el;
  el.style.left = x + 'px';
  el.style.top = y + 'px';
  el.style.width = w + 'px';
  el.style.height = h + 'px';
  el.style.display = 'block';

  /* Apply clip-path to hide portions outside the window bounds.  */
  if (clipT || clipR || clipB || clipL) {
    el.style.clipPath = 'inset('
      + (clipT || 0) + 'px '
      + (clipR || 0) + 'px '
      + (clipB || 0) + 'px '
      + (clipL || 0) + 'px)';
  } else {
    el.style.clipPath = '';
  }

  if (!entry.rendered) {
    entry.rendered = true;
    try {
      entry.renderFn(el);
    } catch (e) {
      console.error('Widget render error (' + id + '):', e);
    }
  }
}

/* Hide widgets that are no longer visible on screen.  */
function hideWidgetsExcept (visibleSet) {
  for (const [id, entry] of widgetRegistry) {
    if (!visibleSet.has(id) && entry.el) {
      entry.el.style.display = 'none';
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

function findMainWindow () {
  const state = window._state;
  if (!state) return null;
  let best = null, bestArea = 0;
  for (const win of state.windows.values()) {
    if (win.menuBar) continue;
    const area = win.w * win.h;
    if (area > bestArea) { bestArea = area; best = win; }
  }
  return best;
}

function windowToPixels (win, excludeModeline) {
  const m = measureFont();
  let h = win.h;
  if (excludeModeline) {
    const lastLine = win.lines && win.lines.get(win.h - 1);
    if (lastLine && lastLine.mode_line) h--;
  }
  return {
    x: Math.round(win.x * m.charW),
    y: Math.round(win.y * m.charH),
    width: Math.ceil(win.w * m.charW),
    height: Math.ceil(h * m.charH),
  };
}

function overlayMainWindow (id) {
  const win = findMainWindow();
  if (!win) return null;
  const px = windowToPixels(win, true);
  return { el: createOverlay(id, px), ...px };
}

function loadScript (url) {
  if (loadedScripts.has(url)) return loadedScripts.get(url);
  const p = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = url;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('Failed to load: ' + url));
    document.head.appendChild(s);
  });
  loadedScripts.set(url, p);
  return p;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

window._widgets = {
  createOverlay,
  destroyOverlay,
  destroyAllOverlays,
  findMainWindow,
  windowToPixels,
  overlayMainWindow,
  loadScript,
  registerWidget,
  positionWidget,
  hideWidgetsExcept,
};
