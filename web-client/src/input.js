/* Input capture for Emacs web display browser client.
   Captures keyboard, mouse, scroll, resize, and focus events
   and sends them as JSON messages over WebSocket.  */

import { measureFont } from './measure.js';

// Modifier bit flags (must match WIRE_MOD_* in webterm.c)
const MOD_SHIFT = 1 << 0;
const MOD_CTRL  = 1 << 2;
const MOD_META  = 1 << 3;
const MOD_SUPER = 1 << 4;

const IS_MAC = /Mac|iPhone|iPad|iPod/.test(navigator.platform)
  || /Mac/.test(navigator.userAgent)
  || navigator.userAgentData?.platform === 'macOS';

function getModifiers (e) {
  let m = 0;
  if (e.shiftKey) m |= MOD_SHIFT;
  if (e.ctrlKey)  m |= MOD_CTRL;
  if (IS_MAC) {
    // macOS: Command = Meta, Option = Super
    if (e.metaKey) m |= MOD_META;
    if (e.altKey)  m |= MOD_SUPER;
  } else {
    // Linux/Windows: Alt = Meta, Win/Super = Super
    if (e.altKey)  m |= MOD_META;
    if (e.metaKey) m |= MOD_SUPER;
  }
  return m;
}

function swallow (e) {
  e.preventDefault();
  e.stopPropagation();
  if (e.stopImmediatePropagation) e.stopImmediatePropagation();
}

// Map browser key names to keycodes Emacs understands.
const SPECIAL_KEYS = {
  'Backspace':  0xFF08,
  'Tab':        0xFF09,
  'Enter':      0xFF0D,
  'Escape':     0xFF1B,
  'Delete':     0xFFFF,
  'Home':       0xFF50,
  'End':        0xFF57,
  'PageUp':     0xFF55,
  'PageDown':   0xFF56,
  'ArrowLeft':  0xFF51,
  'ArrowUp':    0xFF52,
  'ArrowRight': 0xFF53,
  'ArrowDown':  0xFF54,
  'Insert':     0xFF63,
  'F1':  0xFFBE, 'F2':  0xFFBF, 'F3':  0xFFC0, 'F4':  0xFFC1,
  'F5':  0xFFC2, 'F6':  0xFFC3, 'F7':  0xFFC4, 'F8':  0xFFC5,
  'F9':  0xFFC6, 'F10': 0xFFC7, 'F11': 0xFFC8, 'F12': 0xFFC9,
  'F13': 0xFFCA, 'F14': 0xFFCB, 'F15': 0xFFCC, 'F16': 0xFFCD,
  'F17': 0xFFCE, 'F18': 0xFFCF, 'F19': 0xFFD0, 'F20': 0xFFD1,
  'F21': 0xFFD2, 'F22': 0xFFD3, 'F23': 0xFFD4, 'F24': 0xFFD5,
};

/* Map physical key codes (e.code) to the base ASCII character.
   Used on macOS when Option is held, since the browser produces the
   Option-composed character in e.key instead of the raw key.  */
function codeToChar (code, shift) {
  if (code.startsWith('Key')) {
    const ch = code.charCodeAt(3);  // 'A'-'Z'
    return shift ? ch : (ch + 32);  // uppercase or lowercase
  }
  if (code.startsWith('Digit')) {
    if (!shift) return code.charCodeAt(5);  // '0'-'9'
    // Shifted digits on US keyboard
    const shifted = ')!@#$%^&*(';
    const idx = code.charCodeAt(5) - 48;  // '0'=48
    return shifted.charCodeAt(idx);
  }
  // Punctuation keys (US keyboard layout)
  const MAP = {
    Minus:        [0x2D, 0x5F],  // - _
    Equal:        [0x3D, 0x2B],  // = +
    BracketLeft:  [0x5B, 0x7B],  // [ {
    BracketRight: [0x5D, 0x7D],  // ] }
    Backslash:    [0x5C, 0x7C],  // \ |
    Semicolon:    [0x3B, 0x3A],  // ; :
    Quote:        [0x27, 0x22],  // ' "
    Backquote:    [0x60, 0x7E],  // ` ~
    Comma:        [0x2C, 0x3C],  // , <
    Period:       [0x2E, 0x3E],  // . >
    Slash:        [0x2F, 0x3F],  // / ?
    Space:        [0x20, 0x20],
  };
  const entry = MAP[code];
  if (entry) return entry[shift ? 1 : 0];
  return 0;
}

export class InputHandler {
  constructor (el, ws) {
    this.el = el;
    this.ws = ws;
    this._lastMouseMoveTime = 0;
    this._handlers = [];

    this._setupKeyboard();
    this._setupMouse();
    this._setupScroll();
    this._setupFocus();
  }

  _send (obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj) + '\n');
    }
  }

  /* Send a string as individual key events so it flows through
     the normal Emacs command loop (and into web-term's PTY).  */
  _pasteText (text) {
    for (const ch of text) {
      const code = ch.codePointAt(0);
      this._send({ type: 'key', keycode: code, mods: 0, 'char': code });
    }
  }

  _on (target, event, handler, options) {
    target.addEventListener(event, handler, options);
    this._handlers.push({ target, event, handler, options });
  }

  _setupKeyboard () {
    /* Use window + capture phase so we are the FIRST handler for every
       physical keystroke, before Preact or any other delegation.  */
    this._on(window, 'keydown', (e) => {
      swallow(e);

      if (window._state?.activeMenu && e.key === 'Escape') {
        this._send({ type: 'menu_cancel' });
        window._state.dismissMenu();
        return;
      }

      let mods = getModifiers(e);
      let keycode = 0;
      let charCode = 0;

      if (SPECIAL_KEYS[e.key]) {
        keycode = SPECIAL_KEYS[e.key];
        charCode = 0;
        // On macOS, arrow/nav keys have NSEventModifierFlagFunction set,
        // which our native Fn→Option module converts to altKey (= Super).
        // Strip the false Super so plain arrows stay unmodified.
        if (IS_MAC && e.altKey && !e.metaKey && !e.ctrlKey) {
          mods &= ~MOD_SUPER;
        }
      } else if (e.key.length === 1) {
        charCode = e.key.codePointAt(0);
        keycode = charCode;

        // On macOS, a held GUI modifier corrupts e.key in two ways:
        //  - Option (Super for us) composes a special character
        //    (Option+t → "†"), and
        //  - Command (Meta for us) drops the Shift transformation for
        //    punctuation/digits, so Cmd+Shift+',' arrives as ',' not
        //    '<' — which is why M-< came through as M-,.
        // In both cases derive the intended character from the physical
        // key code plus the real shift state.  US-layout map; falls back
        // to e.key when the code isn't mapped.
        if (IS_MAC && (e.altKey || e.metaKey) && e.code) {
          const raw = codeToChar(e.code, e.shiftKey);
          if (raw) {
            charCode = raw;
            keycode = raw;
          }
        }

        // For Ctrl+letter, send the control character
        if (e.ctrlKey && charCode >= 0x61 && charCode <= 0x7A) {
          charCode = charCode - 0x60;
          keycode = charCode;
        } else {
          // Strip shift for regular printable characters — the shift
          // is already incorporated in the character code (e.g. '"'
          // vs "'").  Only special keys need explicit shift_modifier.
          mods &= ~MOD_SHIFT;
        }
      } else if (e.key === 'Dead') {
        // Dead key pressed (e.g. Option+e for acute accent on macOS).
        // When Option is our Super modifier, derive the raw key from
        // the physical key code and send it with Super.
        if (IS_MAC && e.altKey && e.code) {
          const raw = codeToChar(e.code, e.shiftKey);
          if (raw) {
            charCode = raw;
            keycode = raw;
            mods &= ~MOD_SHIFT;
          } else {
            return;
          }
        } else {
          return;
        }
      } else {
        return;
      }

      this._send({ type: 'key', keycode, mods, 'char': charCode });
    }, { capture: true, passive: false });

    const prevent = (e) => swallow(e);
    this._on(window, 'keyup', prevent, { capture: true, passive: false });
    this._on(window, 'keypress', prevent, { capture: true, passive: false });
    this._on(window, 'beforeinput', prevent, { capture: true, passive: false });
    this._on(window, 'contextmenu', prevent, { capture: true, passive: false });
    this._on(window, 'auxclick', prevent, { capture: true, passive: false });
    this._on(window, 'dragstart', prevent, { capture: true, passive: false });
    this._on(window, 'dragover', (e) => { e.preventDefault(); e.dropEffect = 'copy'; },
             { capture: true, passive: false });
    this._on(window, 'drop', (e) => {
      swallow(e);
      const files = e.dataTransfer?.files;
      if (files && files.length > 0) {
        // Use Electron's webUtils preload API for full paths (sandbox-safe),
        // fall back to f.path (non-sandboxed) or f.name.
        const getPath = window.electronAPI?.getPathForFile;
        const paths = Array.from(files).map(f =>
          (getPath ? getPath(f) : null) || f.path || f.name
        ).filter(Boolean);
        if (paths.length > 0) {
          // Use clipboard event for bracketed paste (so terminal apps
          // like Claude Code don't interpret '/' as a command prefix).
          this._send({ type: 'clipboard', dir: 'paste', text: paths.join(' ') });
        }
      }
    }, { capture: true, passive: false });
    this._on(window, 'selectstart', prevent, { capture: true, passive: false });

    this._on(document, 'paste', (e) => {
      swallow(e);
      const text = e.clipboardData?.getData('text');
      if (text) {
        // Use clipboard event for bracketed paste (so terminal apps
        // like Claude Code don't interpret '/' as a command prefix).
        this._send({ type: 'clipboard', dir: 'paste', text });
      }
    }, { capture: true, passive: false });

    /* Ensure the frame element has focus so the page receives keys.  */
    if (this.el) this.el.focus();

    /* Any click anywhere on the page should re-focus the frame element.  */
    this._on(document, 'pointerdown', () => {
      if (this.el) this.el.focus();
    }, { capture: true });
  }

  /* Convert pixel position relative to frame element into Emacs
     pixel coordinates.  Uses fractional browser char dimensions for
     accurate cell detection, then integer Emacs dimensions for the
     pixel values Emacs expects.  */
  _framePixel (e) {
    const rect = this.el.getBoundingClientRect();
    const m = measureFont();
    const px = e.clientX - rect.left;
    const py = e.clientY - rect.top;
    /* Map to character cell using actual browser char size.  */
    const col = Math.floor(px / m.charW);
    const row = Math.floor(py / m.charH);
    /* Convert to Emacs pixel coords using the integer values Emacs knows.  */
    return {
      x: col * m.charWInt,
      y: row * m.charHInt,
    };
  }

  _setupMouse () {
    this._on(this.el, 'mousedown', (e) => {
      e.preventDefault();
      if (this.el) this.el.focus();
      const pos = this._framePixel(e);
      this._send({
        type: 'mouse_down',
        x: pos.x,
        y: pos.y,
        button: e.button,
        mods: getModifiers(e),
      });
    });

    this._on(this.el, 'mouseup', (e) => {
      e.preventDefault();
      const pos = this._framePixel(e);
      this._send({
        type: 'mouse_up',
        x: pos.x,
        y: pos.y,
        button: e.button,
        mods: getModifiers(e),
      });
    });

    this._on(this.el, 'mousemove', (e) => {
      const now = performance.now();
      if (now - this._lastMouseMoveTime < 16) return;
      this._lastMouseMoveTime = now;

      const pos = this._framePixel(e);
      this._send({
        type: 'mouse_move',
        x: pos.x,
        y: pos.y,
        mods: getModifiers(e),
      });
    });

    this._on(this.el, 'contextmenu', (e) => e.preventDefault());
  }

  _setupScroll () {
    this._scrollResidualX = 0;
    this._scrollResidualY = 0;
    this._on(window, 'wheel', (e) => {
      swallow(e);
      const rect = this.el.getBoundingClientRect();
      const inside = e.clientX >= rect.left && e.clientX < rect.right
        && e.clientY >= rect.top && e.clientY < rect.bottom;
      if (!inside) return;

      /* Normalize wheel deltas to pixels.  Trackpads report
         DOM_DELTA_PIXEL, but many mice report DOM_DELTA_LINE (deltaY in
         lines) or DOM_DELTA_PAGE — pixel-scroll-precision-mode needs real
         pixels, so scale those up to the measured line/page height.  */
      const m = measureFont();
      let unitX = 1, unitY = 1;
      if (e.deltaMode === 1) { unitX = m.charW; unitY = m.charH; }
      else if (e.deltaMode === 2) { unitX = rect.width; unitY = rect.height; }

      /* Carry the sub-pixel remainder so fine trackpad deltas accumulate
         instead of rounding to zero (the wire delta is an integer).  */
      this._scrollResidualX += e.deltaX * unitX;
      this._scrollResidualY += e.deltaY * unitY;
      let dx = Math.trunc(this._scrollResidualX);
      let dy = Math.trunc(this._scrollResidualY);
      this._scrollResidualX -= dx;
      this._scrollResidualY -= dy;
      if (dx === 0 && dy === 0) return;

      dx = Math.max(-32768, Math.min(32767, dx));
      dy = Math.max(-32768, Math.min(32767, dy));
      this._send({
        type: 'scroll',
        x: Math.round(e.clientX - rect.left),
        y: Math.round(e.clientY - rect.top),
        dx, dy,
        mods: getModifiers(e),
      });
    }, { passive: false });
  }

  _setupFocus () {
    this._on(window, 'focus', () => {
      this._send({ type: 'focus', gained: true });
      this._syncClipboardIn();
    });
    this._on(window, 'blur', () => {
      this._send({ type: 'focus', gained: false });
    });
    /* Sync once on startup too, so the OS clipboard is available to the
       first yank without needing a focus transition.  */
    this._syncClipboardIn();
  }

  /* Pull the OS clipboard into Emacs (sets web-clipboard-text) so C-y
     yanks text copied in other apps while Emacs was unfocused.  Uses the
     Electron main process; no-op in a plain browser.  */
  async _syncClipboardIn () {
    const read = window.electronAPI?.readClipboard;
    if (!read) return;
    try {
      const text = await read();
      if (typeof text === 'string' && text.length > 0) {
        this._send({ type: 'clipboard', dir: 'paste', text });
      }
    } catch { /* ignore */ }
  }

  destroy () {
    for (const { target, event, handler, options } of this._handlers) {
      target.removeEventListener(event, handler, options);
    }
    this._handlers = [];
  }
}
