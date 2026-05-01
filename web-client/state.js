/* Frame state manager for Emacs web display.
   Stores faces, windows, lines, and cursor state from JSON updates.  */

export class FrameState {
  constructor () {
    /** @type {Map<number, object>} face_id -> {fg, bg, bold, italic, ...} */
    this.faces = new Map();

    /** @type {Map<number, object>} window_id -> {x, y, w, h, lines, cursor} */
    this.windows = new Map();

    /** Default face colors.  */
    this.defaultFg = '#d4d4d4';
    this.defaultBg = '#1e1e1e';

    /** Frame dimensions in character cells.  */
    this.cols = 80;
    this.rows = 50;

    /** Generation counter — incremented on every state change.
        Windows and lines carry their own generation for dirty tracking.  */
    this._gen = 0;

    /** Listeners for state changes.  */
    this._listeners = [];
  }

  onChange (fn) {
    this._listeners.push(fn);
  }

  _notify () {
    for (const fn of this._listeners) fn();
  }

  /** Apply a parsed JSON message from Emacs.  */
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
        this._notify();
        break;
      case 'heartbeat':
        /* Handled by app.js for busy overlay.  */
        break;
      default:
        break;
    }
  }

  dismissMenu () {
    this.activeMenu = null;
    this._notify();
  }

  _applyFrameUpdate (msg) {
    const gen = ++this._gen;

    /* Merge faces.  */
    if (msg.faces) {
      for (const [id, face] of Object.entries(msg.faces)) {
        this.faces.set(Number(id), face);
      }
    }

    /* Merge windows and their dirty lines.  */
    if (msg.windows) {
      for (const wdata of msg.windows) {
        let win = this.windows.get(wdata.id);
        if (!win) {
          win = {
            id: wdata.id,
            x: wdata.x, y: wdata.y,
            w: wdata.w, h: wdata.h,
            lines: new Map(),
            cursor: null,
            menuBar: !!wdata.menu_bar,
            gen,
          };
          this.windows.set(wdata.id, win);
        } else {
          win.x = wdata.x;
          win.y = wdata.y;
          win.w = wdata.w;
          win.h = wdata.h;
          if (wdata.menu_bar !== undefined) win.menuBar = !!wdata.menu_bar;
          win.gen = gen;
        }

        /* Merge dirty lines — stamp each with generation.  */
        if (wdata.lines) {
          for (const lineData of wdata.lines) {
            let row = lineData.row;
            if (lineData.mode_line && row >= win.h) row = win.h - 1;
            if (row >= 0 && row < win.h) {
              win.lines.set(row, { ...lineData, row, gen });
            }
          }
        }

        /* Update cursor.  */
        if (wdata.cursor) {
          win.cursor = {
            ...wdata.cursor,
            row: Math.max(0, Math.min(wdata.cursor.row, win.h - 1)),
          };
        }
      }
    }

    this._notify();
  }

  _applyScroll (msg) {
    const win = this.windows.get(msg.window_id);
    if (!win) return;

    const delta = msg.delta_rows;
    if (delta === 0) return;

    /* Shift existing lines.  */
    const newLines = new Map();
    for (const [row, line] of win.lines) {
      const newRow = row - delta;
      if (newRow >= 0 && newRow < win.h) {
        const shifted = { ...line, row: newRow };
        newLines.set(newRow, shifted);
      }
    }
    win.lines = newLines;

    /* Shift cursor.  */
    if (win.cursor) {
      win.cursor.row -= delta;
    }

    this._notify();
  }

  _applyClear (msg) {
    if (msg.bg) {
      this.defaultBg = msg.bg;
    }
    /* Clear all windows.  */
    this.windows.clear();
    this._notify();
  }

  _applyFrameSize (msg) {
    this.cols = msg.cols || this.cols;
    this.rows = msg.rows || this.rows;
    if (msg.default_face) {
      this.defaultFg = msg.default_face.fg || this.defaultFg;
      this.defaultBg = msg.default_face.bg || this.defaultBg;
    }
    this._notify();
  }

  /** Look up a face by id, returning style properties.  */
  getFace (faceId) {
    return this.faces.get(faceId) || {
      fg: this.defaultFg,
      bg: this.defaultBg,
    };
  }
}
