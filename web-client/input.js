/* Input capture for Emacs web display browser client.
   Captures keyboard, mouse, scroll, resize, and focus events
   and sends them as JSON messages over WebSocket.  */

// Modifier bit flags (match Emacs conventions)
const MOD_SHIFT = 1 << 0;
const MOD_CTRL  = 1 << 2;
const MOD_META  = 1 << 3;  // Alt on PC
const MOD_SUPER = 1 << 4;  // Cmd on Mac

function getModifiers (e) {
  let m = 0;
  if (e.shiftKey) m |= MOD_SHIFT;
  if (e.ctrlKey)  m |= MOD_CTRL;
  if (e.altKey)   m |= MOD_META;
  if (e.metaKey)  m |= MOD_SUPER;
  return m;
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
};

export class InputHandler {
  constructor (el, ws) {
    this.el = el;
    this.ws = ws;
    this._lastMouseMoveTime = 0;
    this._handlers = [];
    this._metrics = null; /* cached font metrics */

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

  _on (target, event, handler, options) {
    target.addEventListener(event, handler, options);
    this._handlers.push({ target, event, handler, options });
  }

  _setupKeyboard () {
    /* Use window + capture phase so we are the FIRST handler for every
       physical keystroke, before Preact or any other delegation.  */
    this._on(window, 'keydown', (e) => {
      e.preventDefault();
      e.stopPropagation();

      const mods = getModifiers(e);
      let keycode = 0;
      let charCode = 0;

      if (SPECIAL_KEYS[e.key]) {
        keycode = SPECIAL_KEYS[e.key];
        charCode = 0;
      } else if (e.key.length === 1) {
        charCode = e.key.codePointAt(0);
        keycode = charCode;
        // For Ctrl+letter, send the control character
        if (e.ctrlKey && charCode >= 0x61 && charCode <= 0x7A) {
          charCode = charCode - 0x60;
          keycode = charCode;
        }
      } else {
        return;
      }

      this._send({ type: 'key', keycode, mods, 'char': charCode });
    }, { capture: true });

    this._on(window, 'keypress', (e) => e.preventDefault(), { capture: true });

    this._on(document, 'paste', (e) => {
      e.preventDefault();
      const text = e.clipboardData?.getData('text');
      if (text) {
        this._send({ type: 'clipboard', dir: 'paste', text });
      }
    });

    /* Ensure the frame element has focus so the page receives keys.  */
    if (this.el) this.el.focus();

    /* Any click anywhere on the page should re-focus the frame element.  */
    this._on(document, 'click', () => {
      if (this.el) this.el.focus();
    }, { capture: true });
  }

  /* Convert pixel position relative to frame element into Emacs
     pixel coordinates using the same char width/height Emacs knows.  */
  _framePixel (e) {
    const rect = this.el.getBoundingClientRect();
    if (!this._metrics) this._measureFont();
    const px = Math.round(e.clientX - rect.left);
    const py = Math.round(e.clientY - rect.top);
    /* Send coordinates as multiples of the char cell that Emacs uses,
       scaled back to the pixel values Emacs expects.  This eliminates
       rounding mismatches between browser font rendering and Emacs's
       hardcoded char dimensions.  */
    const col = Math.floor(px / this._metrics.charW);
    const row = Math.floor(py / this._metrics.charH);
    return {
      x: col * this._metrics.charW,
      y: row * this._metrics.charH,
    };
  }

  _measureFont () {
    const el = document.createElement('span');
    el.style.fontFamily = 'monospace';
    el.style.fontSize = '16px';
    el.style.position = 'absolute';
    el.style.visibility = 'hidden';
    el.style.whiteSpace = 'pre';
    el.textContent = 'M';
    document.body.appendChild(el);
    const rect = el.getBoundingClientRect();
    this._metrics = {
      charW: Math.ceil(rect.width),
      charH: Math.ceil(rect.height * 1.2),
    };
    document.body.removeChild(el);
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
    this._on(this.el, 'wheel', (e) => {
      e.preventDefault();
      const rect = this.el.getBoundingClientRect();
      let dx = Math.round(e.deltaX);
      let dy = Math.round(e.deltaY);
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
    });
    this._on(window, 'blur', () => {
      this._send({ type: 'focus', gained: false });
    });
  }

  destroy () {
    for (const { target, event, handler, options } of this._handlers) {
      target.removeEventListener(event, handler, options);
    }
    this._handlers = [];
  }
}
