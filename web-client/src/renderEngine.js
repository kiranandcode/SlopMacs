/* Canvas glyph-atlas renderer for the Emacs web display.
   The hot path mirrors Monaco's model: stable surface, rAF batching,
   cached glyphs, and direct painting outside React.  */

import { FONT_FAMILY, FONT_SIZE } from './measure.js';

const CURSOR_CLASS = ['cursor-box', 'cursor-hollow', 'cursor-bar', 'cursor-hbar'];

function escapeHtml (s) {
  if (s.indexOf('&') === -1 && s.indexOf('<') === -1 && s.indexOf('>') === -1)
    return s;
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function charsOf (text) {
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) return Array.from(text);
  }
  return text;
}

function menuItemsFromWindow (win) {
  const lineData = win.lines.get(0);
  if (!lineData || !lineData.runs) return [];

  let fullText = '';
  for (const run of lineData.runs) fullText += run.text || '';

  const items = [];
  let i = 0;
  while (i < fullText.length) {
    if (fullText[i] === ' ') {
      i++;
      continue;
    }
    const start = i;
    while (i < fullText.length && fullText[i] !== ' ') i++;
    items.push({ label: fullText.substring(start, i), col: start });
  }
  return items;
}

function quoteFontFamily (family) {
  return "'" + String(family).replace(/\\/g, '\\\\').replace(/'/g, "\\'") + "'";
}

function fontForFace (face, dpr) {
  const style = face.italic ? 'italic ' : '';
  const weight = face.bold ? '700 ' : '400 ';
  const family = face.family
    ? quoteFontFamily(face.family) + ',' + FONT_FAMILY
    : FONT_FAMILY;
  return style + weight + Math.round(FONT_SIZE * dpr) + 'px ' + family;
}

function cursorClass (cursor) {
  if (!cursor.active) return 'cursor-inactive';
  return CURSOR_CLASS[cursor.type] || 'cursor-box';
}

class GlyphAtlas {
  constructor (metrics) {
    this.metrics = metrics;
    this.dpr = window.devicePixelRatio || 1;
    this.size = 2048;
    this.pad = Math.max(2, Math.ceil(this.dpr));
    this.cellW = Math.ceil(metrics.charW * this.dpr) + this.pad * 2;
    this.cellH = Math.ceil(metrics.charH * this.dpr) + this.pad * 2;
    this.x = 0;
    this.y = 0;
    this.rowH = this.cellH;
    this.cache = new Map();

    this.canvas = document.createElement('canvas');
    this.canvas.width = this.size;
    this.canvas.height = this.size;
    this.ctx = this.canvas.getContext('2d', { alpha: true });
    this.ctx.textBaseline = 'alphabetic';
    this.ctx.imageSmoothingEnabled = false;
  }

  clear () {
    this.ctx.clearRect(0, 0, this.size, this.size);
    this.x = 0;
    this.y = 0;
    this.rowH = this.cellH;
    this.cache.clear();
  }

  get (ch, face) {
    const key = ch + '\0' + face.fg + '\0'
      + (face.bold ? 1 : 0) + '\0' + (face.italic ? 1 : 0)
      + '\0' + (face.family || '');
    const cached = this.cache.get(key);
    if (cached) return cached;

    if (this.x + this.cellW > this.size) {
      this.x = 0;
      this.y += this.rowH;
    }
    if (this.y + this.cellH > this.size) this.clear();

    const g = {
      sx: this.x,
      sy: this.y,
      sw: this.cellW,
      sh: this.cellH,
      dx: this.pad / this.dpr,
      dy: this.pad / this.dpr,
      dw: this.cellW / this.dpr,
      dh: this.cellH / this.dpr,
    };

    this.ctx.font = fontForFace(face, this.dpr);
    this.ctx.fillStyle = face.fg;
    this.ctx.clearRect(g.sx, g.sy, g.sw, g.sh);
    this.ctx.fillText(
      ch,
      g.sx + this.pad,
      g.sy + this.pad + this.metrics.ascent * this.dpr
    );

    this.cache.set(key, g);
    this.x += this.cellW;
    return g;
  }
}

class MenuRecord {
  constructor (node) {
    this.node = node;
    this.geomKey = '';
    this.menuKey = '';
  }
}

export class FrameRenderer {
  constructor (root, metrics) {
    this.root = root;
    this.metrics = metrics;
    this.dpr = window.devicePixelRatio || 1;
    this.pendingState = null;
    this.lastState = null;
    this.raf = 0;
    this.width = 0;
    this.height = 0;
    this.menuBars = new Map();
    this.cursors = new Map();

    /* Dirty tracking: per-window map of row → last-rendered gen.  */
    this.renderedLineGens = new Map();
    /* Per-window cursor state for dirty tracking.  */
    this.renderedCursorGens = new Map();
    /* Track last-rendered face generation.  */
    this.renderedFaceGen = -1;
    /* Force full repaint on next render (resize, clear, etc.).  */
    this.fullRepaint = true;
    /* Layout key: detect when window geometry changes.  */
    this.layoutKey = '';

    this.canvas = document.createElement('canvas');
    this.canvas.className = 'emacs-text-canvas';
    this.ctx = this.canvas.getContext('2d', {
      alpha: false,
      desynchronized: true,
    });
    this.ctx.imageSmoothingEnabled = false;

    this.atlas = new GlyphAtlas(metrics);

    root.textContent = '';
    root.appendChild(this.canvas);
    root.style.setProperty('--char-w', metrics.charW + 'px');
    root.style.setProperty('--char-h', metrics.charH + 'px');

    this.resizeObserver = new ResizeObserver(() => {
      this.fullRepaint = true;
      if (this.lastState) this.schedule(this.lastState);
    });
    this.resizeObserver.observe(root);
  }

  destroy () {
    if (this.raf) cancelAnimationFrame(this.raf);
    this.raf = 0;
    this.resizeObserver.disconnect();
    this.menuBars.clear();
    this.cursors.clear();
    this.root.textContent = '';
  }

  /* Deferred render via rAF — use for scroll, resize, clear_frame.  */
  schedule (state) {
    this.pendingState = state;
    this.lastState = state;
    if (this.raf) return;
    this.raf = requestAnimationFrame(() => {
      this.raf = 0;
      const next = this.pendingState;
      this.pendingState = null;
      if (next) this.render(next);
    });
  }

  /* Synchronous render — use for frame_update (typing).
     Skips rAF delay for immediate visual feedback.  */
  renderNow (state) {
    this.lastState = state;
    this.pendingState = null;
    if (this.raf) {
      cancelAnimationFrame(this.raf);
      this.raf = 0;
    }
    this.render(state);
  }

  ensureCanvasSize () {
    const rect = this.root.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.ceil(rect.width));
    const height = Math.max(1, Math.ceil(rect.height));

    if (width === this.width && height === this.height && dpr === this.dpr)
      return false;

    this.width = width;
    this.height = height;
    this.dpr = dpr;
    this.canvas.width = Math.ceil(width * dpr);
    this.canvas.height = Math.ceil(height * dpr);
    this.canvas.style.width = width + 'px';
    this.canvas.style.height = height + 'px';
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.ctx.imageSmoothingEnabled = false;
    this.atlas = new GlyphAtlas(this.metrics);
    this.fullRepaint = true;
    return true;
  }

  render (state) {
    this.ensureCanvasSize();
    this.root.style.backgroundColor = state.defaultBg;
    this.root.style.color = state.defaultFg;

    /* If faces changed, all lines need repaint since colors may differ.  */
    if (state._faceGen !== this.renderedFaceGen) {
      this.renderedFaceGen = state._faceGen;
      this.fullRepaint = true;
    }

    /* Detect window layout changes (splits, resizes, deletions).  */
    const windows = [...state.windows.values()].sort((a, b) => {
      if (a.y !== b.y) return a.y - b.y;
      return a.x - b.x;
    });
    let layoutKey = '';
    for (const w of windows)
      layoutKey += w.id + ',' + w.x + ',' + w.y + ',' + w.w + ',' + w.h
        + ',' + (w.px || 0) + ',' + (w.py || 0) + ',' + (w.pw || 0) + ',' + (w.ph || 0) + ';';
    if (layoutKey !== this.layoutKey) {
      this.layoutKey = layoutKey;
      this.fullRepaint = true;
    }

    /* Scrolled windows need full repaint since row content shifted.  */
    if (state.pendingScrolls.length > 0) {
      for (const scroll of state.pendingScrolls) {
        const lineGens = this.renderedLineGens.get(scroll.windowId);
        if (lineGens) lineGens.clear();
      }
      state.pendingScrolls.length = 0;
    }

    const ctx = this.ctx;
    const full = this.fullRepaint;
    this.fullRepaint = false;

    if (full) {
      ctx.fillStyle = state.defaultBg;
      ctx.fillRect(0, 0, this.width, this.height);
      this.renderedLineGens.clear();
      this.renderedCursorGens.clear();
      state.pendingClears.length = 0;
    } else if (state.pendingClears.length > 0) {
      this.applyClearAreas(state);
    }

    const liveMenus = new Set();
    const liveCursors = new Set();
    const visibleWidgets = new Set();

    for (const win of windows) {
      if (win.menuBar) {
        liveMenus.add(win.id);
        this.renderMenuBar(win);
      } else {
        this.drawWindow(win, state, full, visibleWidgets);
        if (win.cursor) {
          liveCursors.add(win.id);
          this.renderCursor(win);
        }
      }
    }

    /* Hide widgets that are no longer visible in any window.  */
    if (window._widgets) window._widgets.hideWidgetsExcept(visibleWidgets);

    for (const [id, rec] of this.menuBars) {
      if (liveMenus.has(id)) continue;
      rec.node.remove();
      this.menuBars.delete(id);
    }

    for (const [id, node] of this.cursors) {
      if (liveCursors.has(id)) continue;
      node.remove();
      this.cursors.delete(id);
    }

    /* Prune dirty-tracking maps for deleted windows.  */
    for (const id of this.renderedLineGens.keys()) {
      if (!state.windows.has(id)) this.renderedLineGens.delete(id);
    }
    for (const id of this.renderedCursorGens.keys()) {
      if (!state.windows.has(id)) this.renderedCursorGens.delete(id);
    }
  }

  drawWindow (win, state, full, visibleWidgets) {
    const m = this.metrics;
    /* Use Emacs pixel coordinates when available for accurate
       positioning (e.g. tab bar with non-standard height).  */
    const x = win.px !== undefined ? win.px : Math.round(win.x * m.charW);
    const y = win.py !== undefined ? win.py : Math.round(win.y * m.charH);
    const w = win.pw !== undefined ? win.pw : Math.ceil(win.w * m.charW);
    const h = win.ph !== undefined ? win.ph : Math.ceil(win.h * m.charH);
    const ctx = this.ctx;

    /* Get or create per-window line generation map.  */
    let lineGens = this.renderedLineGens.get(win.id);
    if (!lineGens) {
      lineGens = new Map();
      this.renderedLineGens.set(win.id, lineGens);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();

    for (let row = 0; row < win.h; row++) {
      const lineData = win.lines.get(row);
      const lineGen = lineData ? lineData.gen : -1;
      const prevGen = lineGens.get(row);

      /* Skip unchanged lines.  */
      if (!full && lineGen === prevGen && lineGen !== -1) continue;

      this.drawLine(win, row, state);
      lineGens.set(row, lineGen);
    }

    this.drawImages(win, state, visibleWidgets);

    ctx.fillStyle = 'rgba(255,255,255,0.06)';
    ctx.fillRect(x + w - 1, y, 1, win.ph || h);
    ctx.restore();
  }

  applyClearAreas (state) {
    const m = this.metrics;
    const ctx = this.ctx;
    for (const clear of state.pendingClears) {
      ctx.fillStyle = clear.bg || state.defaultBg;
      ctx.fillRect(clear.x, clear.y, clear.w, clear.h);

      const clearRight = clear.x + clear.w;
      const clearBottom = clear.y + clear.h;
      for (const win of state.windows.values()) {
        const winX = win.px !== undefined ? win.px : Math.round(win.x * m.charW);
        const winY = win.py !== undefined ? win.py : Math.round(win.y * m.charH);
        const winW = win.pw !== undefined ? win.pw : Math.ceil(win.w * m.charW);
        const winH = win.ph !== undefined ? win.ph : Math.ceil(win.h * m.charH);
        const ix0 = Math.max(clear.x, winX);
        const iy0 = Math.max(clear.y, winY);
        const ix1 = Math.min(clearRight, winX + winW);
        const iy1 = Math.min(clearBottom, winY + winH);
        if (ix0 >= ix1 || iy0 >= iy1) continue;

        const lineGens = this.renderedLineGens.get(win.id);
        if (lineGens) {
          const first = Math.max(0, Math.floor((iy0 - winY) / m.charH) - 1);
          const last = Math.min(win.h - 1,
            Math.ceil((iy1 - winY) / m.charH) + 1);
          for (let row = first; row <= last; row++) lineGens.delete(row);
        }
        this.renderedCursorGens.delete(win.id);
      }
    }
    state.pendingClears.length = 0;
  }

  lineGeometry (win, row, lineData) {
    const m = this.metrics;
    const winX = win.px !== undefined ? win.px : Math.round(win.x * m.charW);
    const winY = win.py !== undefined ? win.py : Math.round(win.y * m.charH);
    const width = win.pw !== undefined ? win.pw : Math.ceil(win.w * m.charW);
    let rowY;
    let rowH;

    if (lineData && lineData.mode_line) {
      const winH = win.ph !== undefined ? win.ph : win.h * m.charH;
      rowY = winH - m.charH;
      rowH = m.charH;
    } else {
      rowY = lineData && Number.isFinite(lineData.pixel_y)
        ? lineData.pixel_y
        : row * m.charH;
      rowH = lineData && Number.isFinite(lineData.pixel_h)
        ? lineData.pixel_h
        : m.charH;
    }

    return {
      x: winX,
      y: winY + rowY,
      width,
      height: Math.ceil(Math.max(rowH, m.charH)),
      rowY,
    };
  }

  drawLine (win, row, state) {
    const m = this.metrics;
    const ctx = this.ctx;
    const lineData = win.lines.get(row);
    const { x, y, width, height } = this.lineGeometry(win, row, lineData);

    const baseBg = lineData && lineData.mode_line && lineData.runs?.[0]
      ? state.getFace(lineData.runs[0].face_id).bg
      : state.defaultBg;
    ctx.fillStyle = baseBg;
    ctx.fillRect(x, y, width, height);

    if (!lineData || !lineData.runs) return;

    let col = 0;
    for (const run of lineData.runs) {
      const face = state.getFace(run.face_id);
      const text = run.text || '';
      const chars = charsOf(text);
      const len = chars.length;
      const rx = x + col * m.charW;
      const rw = len * m.charW;

      ctx.fillStyle = face.bg || state.defaultBg;
      ctx.fillRect(rx, y, Math.ceil(rw), height);

      if (run.img_id > 0) {
        const imgData = state.images.get(run.img_id);
        if (!imgData || !imgData.el.complete || imgData.loading) {
          ctx.fillStyle = 'rgba(128,128,128,0.3)';
          ctx.fillRect(rx, y, Math.ceil(rw), height);
        }
        col += len;
        continue;
      }

      for (let i = 0; i < len; i++) {
        const ch = chars[i];
        if (ch !== ' ') {
          const g = this.atlas.get(ch, face);
          ctx.drawImage(
            this.atlas.canvas,
            g.sx, g.sy, g.sw, g.sh,
            rx + i * m.charW - g.dx,
            y - g.dy,
            g.dw,
            g.dh
          );
        }
      }

      if (face.underline) {
        ctx.fillStyle = face.fg;
        ctx.fillRect(rx, y + height - 2, Math.ceil(rw), 1);
      }
      if (face.strike) {
        ctx.fillStyle = face.fg;
        ctx.fillRect(rx, y + Math.floor(height / 2), Math.ceil(rw), 1);
      }
      if (face.box && face.box !== 'raised' && face.box !== 'lowered') {
        ctx.strokeStyle = typeof face.box === 'string' ? face.box : face.fg;
        ctx.strokeRect(rx + 0.5, y + 0.5, Math.max(0, rw - 1), height - 1);
      }

      col += len;
    }

    if (lineData.mode_line) {
      ctx.fillStyle = 'rgba(255,255,255,0.18)';
      ctx.fillRect(x, y, width, 1);
      ctx.fillStyle = 'rgba(0,0,0,0.35)';
      ctx.fillRect(x, y + height - 1, width, 1);
    }
  }

  drawImages (win, state, visibleWidgets) {
    const m = this.metrics;
    const ctx = this.ctx;
    const winX = win.px !== undefined ? win.px : Math.round(win.x * m.charW);
    const winY = win.py !== undefined ? win.py : Math.round(win.y * m.charH);
    const winW = win.pw !== undefined ? win.pw : Math.ceil(win.w * m.charW);
    const winH = win.ph !== undefined ? win.ph : Math.ceil(win.h * m.charH);
    const widgets = window._widgets;

    /* Compute the window content area (exclude modeline).  */
    const lastLine = win.lines.get(win.h - 1);
    const contentH = (lastLine && lastLine.mode_line) ? winH - m.charH : winH;
    const winRight = winX + winW;
    const winBottom = winY + contentH;

    for (const [row, lineData] of win.lines) {
      if (row < 0 || row >= win.h || !lineData || !lineData.runs) continue;
      const { y } = this.lineGeometry(win, row, lineData);
      let col = 0;

      for (const run of lineData.runs) {
        const text = run.text || '';
        const len = charsOf(text).length;

        if (run.img_id > 0) {
          /* Check if this image is a widget placeholder.  */
          const widgetId = state.widgetImages
            && state.widgetImages.get(run.img_id);
          if (widgetId && widgets) {
            /* Position widget at full size but clip to window area.
               Full dims are needed so D3 etc. can read correct
               offsetWidth/offsetHeight during render.  */
            const wx = winX + col * m.charW;
            const wy = y;
            const imgW = run.img_w || state.images.get(run.img_id)?.w || 0;
            const imgH = run.img_h || state.images.get(run.img_id)?.h || 0;
            const clipTop = Math.max(0, winY - wy);
            const clipRight = Math.max(0, (wx + imgW) - winRight);
            const clipBottom = Math.max(0, (wy + imgH) - winBottom);
            const clipLeft = Math.max(0, winX - wx);
            widgets.positionWidget(widgetId, wx, wy, imgW, imgH,
              clipTop, clipRight, clipBottom, clipLeft);
            visibleWidgets.add(widgetId);
          } else {
            const imgData = state.images.get(run.img_id);
            if (imgData && imgData.el.complete && !imgData.loading) {
              ctx.drawImage(
                imgData.el,
                winX + col * m.charW,
                y,
                run.img_w || imgData.w,
                run.img_h || imgData.h
              );
            }
          }
        }

        col += len;
      }
    }
  }

  renderCursor (win) {
    const cursor = win.cursor;
    if (!cursor) return;

    const prevCursorGen = this.renderedCursorGens.get(win.id);
    if (prevCursorGen === win.cursorGen)
      return;
    this.renderedCursorGens.set(win.id, win.cursorGen);

    const m = this.metrics;
    let node = this.cursors.get(win.id);
    if (!node) {
      node = document.createElement('div');
      node.className = 'emacs-cursor';
      this.root.appendChild(node);
      this.cursors.set(win.id, node);
    }

    node.className = 'emacs-cursor ' + cursorClass(cursor);
    const lineData = win.lines.get(cursor.row);
    const geom = this.lineGeometry(win, cursor.row, lineData);
    const cursorX = geom.x + cursor.col * m.charW;
    node.style.transform = 'translate3d('
      + Math.round(cursorX) + 'px,'
      + Math.round(geom.y) + 'px,0)';
    node.style.width = Math.ceil(m.charW) + 'px';
    node.style.height = Math.ceil(m.charH) + 'px';
  }

  renderMenuBar (win) {
    let rec = this.menuBars.get(win.id);
    if (!rec) {
      const node = document.createElement('div');
      node.className = 'menu-bar';
      node.onmousedown = e => e.stopPropagation();
      node.onmouseup = e => e.stopPropagation();
      node.onclick = (event) => {
        const item = event.target.closest('.menubar-item');
        const ws = window._ws;
        if (!item || !ws || ws.readyState !== WebSocket.OPEN) return;

        const col = Number(item.dataset.col || 0);
        const m = this.metrics;
        const x = Math.round((win.x + col + 1) * m.charWInt);
        const y = Math.round(win.y * m.charHInt + Math.floor(m.charHInt / 2));
        ws.send(JSON.stringify({ type: 'mouse_down', x, y, button: 0, mods: 0 }) + '\n');
        ws.send(JSON.stringify({ type: 'mouse_up', x, y, button: 0, mods: 0 }) + '\n');
      };
      this.root.appendChild(node);
      rec = new MenuRecord(node);
      this.menuBars.set(win.id, rec);
    }

    const m = this.metrics;
    const geomKey = win.x + ':' + win.y + ':' + win.w + ':' + win.h
      + ':' + (win.px || 0) + ':' + (win.py || 0);
    if (rec.geomKey !== geomKey) {
      rec.geomKey = geomKey;
      const mbX = win.px !== undefined ? win.px : Math.round(win.x * m.charW);
      const mbY = win.py !== undefined ? win.py : Math.round(win.y * m.charH);
      const mbW = win.pw !== undefined ? win.pw : Math.ceil(win.w * m.charW);
      const mbH = win.ph !== undefined ? win.ph : Math.ceil(win.h * m.charH);
      rec.node.style.transform = 'translate3d(' + mbX + 'px,' + mbY + 'px,0)';
      rec.node.style.width = mbW + 'px';
      rec.node.style.height = mbH + 'px';
    }

    const items = menuItemsFromWindow(win);
    const menuKey = win.gen + ':' + items.length + ':'
      + items.map(item => item.col + '=' + item.label).join('|');
    if (rec.menuKey === menuKey) return;
    rec.menuKey = menuKey;

    rec.node.innerHTML = items.map(item =>
      '<div class="menubar-item" data-col="' + item.col + '">'
      + escapeHtml(item.label) + '</div>'
    ).join('');
  }
}
