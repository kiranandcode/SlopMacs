/* Frame state manager for Emacs web display.
   Uses immutable window/line objects so React.memo can detect changes.  */

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
  }

  onChange (fn) {
    this._listeners.push(fn);
    return () => {
      const idx = this._listeners.indexOf(fn);
      if (idx >= 0) this._listeners.splice(idx, 1);
    };
  }

  _notify (immediate) {
    for (const fn of this._listeners) fn(immediate);
  }

  dispatch (msg) {
    switch (msg.type) {
      case 'frame_update':
        this._applyFrameUpdate(msg);
        break;
      case 'scroll':
        this._applyScroll(msg);
        break;
      case 'clear_frame':
        this._applyClear(msg);
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
        /* Only copy lines Map if we have new line data.  */
        const lines = wdata.lines && wdata.lines.length > 0
          ? (old ? new Map(old.lines) : new Map())
          : (old ? old.lines : new Map());
        let cursor = old ? old.cursor : null;
        let cursorGen = old ? old.cursorGen : 0;

        if (wdata.lines) {
          for (const lineData of wdata.lines) {
            let row = lineData.row;
            if (lineData.mode_line) {
              /* Mode line always goes at the last row.  Remove any
                 stale mode_line entries at other positions first.  */
              const h = wdata.h || (old && old.h) || 50;
              row = h - 1;
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

        /* Prune lines beyond the new window height.  */
        for (const row of lines.keys()) {
          if (row >= newH) lines.delete(row);
        }

        this.windows.set(wdata.id, {
          id: wdata.id,
          x: wdata.x ?? (old ? old.x : 0),
          y: wdata.y ?? (old ? old.y : 0),
          w: wdata.w ?? (old ? old.w : 80),
          h: newH,
          lines,
          cursor,
          cursorGen,
          menuBar: wdata.menu_bar !== undefined
            ? !!wdata.menu_bar
            : (old ? old.menuBar : false),
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
        /* Clear stale pixel_y/pixel_h so drawLine falls back to
           grid positioning (row * charH) until the next frame_update
           from Emacs provides correct pixel coordinates.  */
        const { pixel_y, pixel_h, ...rest } = line;
        lines.set(newRow, { ...rest, row: newRow, gen });
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

    this.windows.set(msg.window_id, {
      ...old,
      lines,
      cursor,
      cursorGen: gen,
      gen,
    });

    this._notify(false);
  }

  _applyClear (msg) {
    if (msg.bg) {
      this.defaultBg = msg.bg;
    }
    this.windows.clear();
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
