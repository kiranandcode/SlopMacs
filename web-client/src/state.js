/* Frame state manager for Emacs web display.  */

import { measureFont } from './measure.js';

export class FrameState {
  constructor () {
    this.faces = new Map();
    this.windows = new Map();
    this.images = new Map();
    this.widgetImages = new Map(); /* img_id → widget-id */
    this.defaultFg = '#d4d4d4';
    this.defaultBg = '#1e1e1e';
    this.cols = 80;
    this.rows = 50;
    this._gen = 0;
    this._faceGen = 0;
    this._listeners = [];
    this.activeMenu = null;
    this.hasFirstFrame = false;
    this.pendingScrolls = [];
    this.pendingClears = [];
    this._clearPending = false;
  }

  onChange (fn) {
    this._listeners.push(fn);
    return () => {
      const idx = this._listeners.indexOf(fn);
      if (idx >= 0) this._listeners.splice(idx, 1);
    };
  }

  _notify (immediate) {
    /* A listener that throws must not starve the listeners after it.  */
    for (const fn of this._listeners) {
      try { fn(immediate); } catch (e) { console.error('state listener:', e); }
    }
  }

  dispatch (msg) {
    switch (msg.type) {
      case 'clipboard':
        /* Emacs copied (M-w / C-w) -> write the OS clipboard.  Only the
           'copy' direction arrives here; 'paste' flows the other way
           (browser paste event -> Emacs).  Prefer the Electron main
           process (no focus/gesture requirement); fall back to the
           renderer Clipboard API, which only works when the window is
           focused.  */
        if (msg.dir === 'copy' && typeof msg.text === 'string') {
          if (window.electronAPI?.writeClipboard) {
            Promise.resolve(window.electronAPI.writeClipboard(msg.text))
              .catch(() => {});
          } else if (navigator.clipboard?.writeText) {
            navigator.clipboard.writeText(msg.text).catch(() => {});
          }
        }
        break;
      case 'frame_update':
        this._applyFrameUpdate(msg);
        break;
      case 'scroll':
        this._applyScroll(msg);
        break;
      case 'clear_frame':
        this._applyClear(msg);
        break;
      case 'clear_area':
        this._applyClearArea(msg);
        break;
      case 'frame_size':
        this._applyFrameSize(msg);
        break;
      case 'menu':
        this.activeMenu = msg;
        this._notify(false);
        break;
      case 'load_font': {
        const font = new FontFace(msg.name,
          `url(data:font/ttf;base64,${msg.data})`);
        font.load().then(f => {
          document.fonts.add(f);
          this._faceGen++;
          this._notify(false);
        }).catch(() => {});
        break;
      }
      case 'image_data': {
        /* Detect widget placeholder SVGs: they contain data-widget="ID".  */
        if (msg.mime === 'image/svg+xml' || msg.mime === 'image/svg') {
          try {
            const svgText = atob(msg.data);
            const match = svgText.match(/data-widget="([^"]+)"/);
            if (match) {
              this.widgetImages.set(msg.id, match[1]);
            }
          } catch (e) { /* ignore decode errors */ }
        }
        const img = new Image();
        this.images.set(msg.id, {
          el: img,
          w: msg.width,
          h: msg.height,
          loading: true,
        });
        img.onload = () => {
          this.images.set(msg.id, { el: img, w: msg.width, h: msg.height });
          this._notify(false);
        };
        img.onerror = () => {
          this.images.delete(msg.id);
          this._notify(false);
        };
        img.src = `data:${msg.mime};base64,${msg.data}`;
        break;
      }
      default:
        break;
    }
  }

  dismissMenu () {
    this.activeMenu = null;
    this._notify(false);
  }

  _applyFrameUpdate (msg) {
    const gen = ++this._gen;
    this.hasFirstFrame = true;

    if (msg.faces) {
      let faceChanged = false;
      for (const [id, face] of Object.entries(msg.faces)) {
        const fid = Number(id);
        const old = this.faces.get(fid);
        if (!old || old.fg !== face.fg || old.bg !== face.bg
            || old.bold !== face.bold || old.italic !== face.italic
            || old.underline !== face.underline || old.strike !== face.strike
            || old.family !== face.family) {
          this.faces.set(fid, face);
          faceChanged = true;
        }
      }
      if (faceChanged) {
        this._faceGen++;
        /* Keep defaultBg/Fg in sync with the default face (id 0).  */
        const face0 = this.faces.get(0);
        if (face0) {
          if (face0.bg) this.defaultBg = face0.bg;
          if (face0.fg) this.defaultFg = face0.fg;
        }
      }
    }

    if (msg.windows) {
      for (const wdata of msg.windows) {
        const old = this.windows.get(wdata.id);
        /* Pixel strips vacated by evicted ghost lines; the renderer
           erases and then empties this list.  */
        const vacated = old && old.vacated ? old.vacated : [];
        /* Only copy lines Map if we have new line data.  */
        const lines = wdata.lines && wdata.lines.length > 0
          ? (old ? new Map(old.lines) : new Map())
          : (old ? old.lines : new Map());
        let cursor = old ? old.cursor : null;
        let cursorGen = old ? old.cursorGen : 0;

        if (wdata.lines) {
          for (const lineData of wdata.lines) {
            const row = lineData.row;
            if (lineData.mode_line) {
              /* Trust the matrix vpos — it can be h itself when a
                 partially-visible text row sits above the mode line.
                 Rendering pins mode lines to the window bottom by
                 flag, not index.  Drop stale entries elsewhere.  */
              for (const [r, ld] of lines) {
                if (ld.mode_line && r !== row) lines.delete(r);
              }
            }
            if (row >= 0) {
              lines.set(row, { ...lineData, row, gen });
            }
          }
        }

        if (wdata.cursor) {
          const h = wdata.h || (old && old.h) || 50;
          cursor = {
            ...wdata.cursor,
            row: Math.max(0, Math.min(wdata.cursor.row, h - 1)),
          };
          cursorGen = gen;
        }

        const newH = wdata.h ?? (old ? old.h : 50);

        /* Prune lines beyond the new window height.  The mode line
           may legitimately sit at index newH (after a partial row),
           so it is only pruned past that.  */
        for (const [row, ld] of lines) {
          if (ld.mode_line ? row > newH : row >= newH) lines.delete(row);
        }

        /* Pixel-overlap invariant: lines are keyed by matrix row
           index but PAINTED at pixel_y, and the index<->pixel mapping
           shifts under scrolls and variable-height rows — every merge
           path (partial update, scroll translation, cross-flush
           staleness) can leave two lines claiming the same pixels,
           which renders as ghost copies or content stomped by stale
           empties.  Enforce it globally after each merge.  */
        this._dedupeOverlaps(lines, vacated);

        this.windows.set(wdata.id, {
          id: wdata.id,
          x: wdata.x ?? (old ? old.x : 0),
          y: wdata.y ?? (old ? old.y : 0),
          w: wdata.w ?? (old ? old.w : 80),
          h: newH,
          px: wdata.px ?? (old ? old.px : undefined),
          py: wdata.py ?? (old ? old.py : undefined),
          pw: wdata.pw ?? (old ? old.pw : undefined),
          ph: wdata.ph ?? (old ? old.ph : undefined),
          lines,
          cursor,
          cursorGen,
          menuBar: wdata.menu_bar !== undefined
            ? !!wdata.menu_bar
            : (old ? old.menuBar : false),
          /* Embedded browser view: URL overlaid as an iframe over the
             window body.  Emacs always sends the field ('' = none), so
             absence means "window not in this update — keep".  */
          webview: wdata.webview !== undefined
            ? wdata.webview
            : (old ? old.webview : ''),
          webviewPh: wdata.webview_ph !== undefined
            ? wdata.webview_ph
            : (old ? old.webviewPh : 0),
          vacated,
          gen,
        });
      }
    }

    /* Prune windows deleted on the Emacs side.  */
    if (msg.all_windows) {
      const live = new Set(msg.all_windows);
      for (const id of this.windows.keys()) {
        if (!live.has(id)) this.windows.delete(id);
      }
    }

    this._notify(true);
  }

  /* Enforce: no two non-mode-line lines overlap in pixel space.
     Walk lines sorted by pixel_y; on overlap keep the newer line
     (higher gen) and record the loser's strip in VACATED so the
     renderer erases its pixels where they actually are.  */
  _dedupeOverlaps (lines, vacated) {
    const sorted = [...lines.entries()]
      .filter(([, l]) => !l.mode_line && Number.isFinite(l.pixel_y))
      .sort((a, b) => a[1].pixel_y - b[1].pixel_y);
    let prev = null;
    for (const ent of sorted) {
      if (prev
          && ent[1].pixel_y < prev[1].pixel_y + (prev[1].pixel_h || 1)) {
        const loser = (ent[1].gen || 0) >= (prev[1].gen || 0) ? prev : ent;
        const winner = loser === prev ? ent : prev;
        lines.delete(loser[0]);
        vacated.push({ y: loser[1].pixel_y, h: loser[1].pixel_h || 1 });
        prev = winner;
      } else {
        prev = ent;
      }
    }
  }

  _applyScroll (msg) {
    const old = this.windows.get(msg.window_id);
    if (!old) return;

    const gen = ++this._gen;
    const delta = msg.delta_rows;
    if (delta === 0) return;

    const lines = new Map(old.lines);
    const sourceStart = Number.isInteger(msg.current_row) ? msg.current_row : 0;
    const count = Number.isInteger(msg.nrows) ? msg.nrows : old.h;
    const moved = [];

    for (let i = 0; i < count; i++) {
      const row = sourceStart + i;
      const line = old.lines.get(row);
      if (!line || line.mode_line) continue;
      moved.push([row + delta, line]);
    }

    for (let i = 0; i < count; i++) {
      const row = sourceStart + i;
      const line = old.lines.get(row);
      if (line && !line.mode_line) lines.delete(row);
    }

    for (const [newRow, line] of moved) {
      if (newRow >= 0 && newRow < old.h) {
        const movedLine = { ...line, row: newRow, gen };
        if (Number.isInteger(msg.delta_px)
            && Number.isFinite(movedLine.pixel_y)) {
          /* Moved rows are NOT resent by Emacs (that is the point of
             the scroll optimization), so translate their pixel
             position by the block's pixel shift.  Falling back to
             row*charH here is wrong whenever row heights vary
             (images): the moved block then rendered as a ghost copy
             at compacted positions.  */
          movedLine.pixel_y = movedLine.pixel_y + msg.delta_px;
        } else {
          /* No pixel delta available (older Emacs): drop pixel info
             and hope a following frame_update re-supplies it.  */
          delete movedLine.pixel_y;
          delete movedLine.pixel_h;
        }
        lines.set(newRow, movedLine);
      }
    }

    let cursor = old.cursor;
    if (cursor
        && cursor.row >= sourceStart
        && cursor.row < sourceStart + count) {
      const nextRow = cursor.row + delta;
      cursor = nextRow >= 0 && nextRow < old.h
        ? { ...cursor, row: nextRow }
        : null;
    }

    this.pendingScrolls.push({ windowId: msg.window_id });

    /* Translation can land moved rows on unmoved ones.  */
    const vacated = old.vacated || [];
    this._dedupeOverlaps(lines, vacated);

    this.windows.set(msg.window_id, {
      ...old,
      lines,
      cursor,
      cursorGen: gen,
      vacated,
      gen,
    });

    this._notify(false);
  }

  _applyClearArea (msg) {
    const gen = ++this._gen;
    const metrics = measureFont();
    const clear = {
      x: msg.x || 0,
      y: msg.y || 0,
      w: Math.max(0, msg.w || 0),
      h: Math.max(0, msg.h || 0),
      bg: msg.bg || this.defaultBg,
    };

    if (clear.w <= 0 || clear.h <= 0) return;

    this.pendingClears.push(clear);

    const clearRight = clear.x + clear.w;
    const clearBottom = clear.y + clear.h;
    for (const [id, old] of this.windows) {
      const winX = old.px !== undefined ? old.px : Math.round(old.x * metrics.charW);
      const winY = old.py !== undefined ? old.py : Math.round(old.y * metrics.charH);
      const winW = old.pw !== undefined ? old.pw : Math.ceil(old.w * metrics.charW);
      const winH = old.ph !== undefined ? old.ph : Math.ceil(old.h * metrics.charH);
      const ix0 = Math.max(clear.x, winX);
      const iy0 = Math.max(clear.y, winY);
      const ix1 = Math.min(clearRight, winX + winW);
      const iy1 = Math.min(clearBottom, winY + winH);
      if (ix0 >= ix1 || iy0 >= iy1) continue;

      const fullRowClear = (ix1 - ix0) >= Math.max(metrics.charW, winW * 0.5);
      if (!fullRowClear) continue;

      const lines = new Map(old.lines);
      let changed = false;
      for (const [row, line] of old.lines) {
        const rowY = line && line.mode_line
          ? (old.h - 1) * metrics.charH
          : Number.isFinite(line?.pixel_y)
            ? line.pixel_y
            : row * metrics.charH;
        const lineTop = winY + rowY;
        /* Only delete lines whose top edge starts inside the clear
           area.  This removes genuinely vacated rows (e.g. bottom of
           window after scroll-up) while preserving heading lines that
           merely touch the clear area at their bottom boundary.  */
        if (lineTop >= iy0 && lineTop < iy1) {
          lines.delete(row);
          changed = true;
        }
      }

      if (changed) {
        this.windows.set(id, { ...old, lines, gen });
      }
    }

    this._notify(false);
  }

  _applyClear (msg) {
    if (msg.bg) {
      this.defaultBg = msg.bg;
    }
    /* Clear all windows.  The C side guarantees clear_frame is only
       sent when a frame_update with window data follows in the same
       flush batch, so windows will be repopulated immediately.  */
    this.windows.clear();
    this.pendingClears.length = 0;
    this.pendingScrolls.length = 0;
    this._clearPending = false;
    this._gen++;
    this._notify(false);
  }

  _applyFrameSize (msg) {
    this.cols = msg.cols || this.cols;
    this.rows = msg.rows || this.rows;
    if (msg.default_face) {
      this.defaultFg = msg.default_face.fg || this.defaultFg;
      this.defaultBg = msg.default_face.bg || this.defaultBg;
    }
    this._notify(false);
  }

  getFace (faceId) {
    return this.faces.get(faceId) || {
      fg: this.defaultFg,
      bg: this.defaultBg,
    };
  }
}
