/* Frame state manager for Emacs web display.
   Uses immutable window/line objects so React.memo can detect changes.  */

export class FrameState {
  constructor () {
    this.faces = new Map();
    this.windows = new Map();
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
            || old.underline !== face.underline || old.strike !== face.strike) {
          this.faces.set(fid, face);
          faceChanged = true;
        }
      }
      if (faceChanged) this._faceGen++;
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
            if (lineData.mode_line && row >= (wdata.h || (old && old.h) || 50))
              row = (wdata.h || (old && old.h) || 50) - 1;
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

    const lines = new Map();
    for (const [row, line] of old.lines) {
      const newRow = row + delta;
      if (newRow >= 0 && newRow < old.h) {
        lines.set(newRow, { ...line, row: newRow, gen });
      }
    }

    const cursor = old.cursor
      ? { ...old.cursor, row: old.cursor.row + delta }
      : null;

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
