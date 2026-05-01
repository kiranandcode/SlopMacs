# Plan: Emacs Web Display Backend — Async Split Architecture

## Context

We're building a web display backend for Emacs that renders to a browser via WebSocket + HTML5 Canvas. To solve Emacs's single-threaded hang problem, we use a **two-process architecture**: Emacs sends draw commands over a Unix socketpair to a separate **Display Proxy** process that owns the WebSocket server and never blocks.

## Current State — Phase 1 COMPLETE

The skeleton is already implemented and compiles with `--with-web`:

**Existing files (already created):**
- `src/webterm.h` — `struct web_display_info` (22 fields), `struct web_output` (21 fields), macros
- `src/webgui.h` — X-compatible type definitions, rectangle macros, gravity constants
- `src/webterm.c` — 43 stub RIF functions, 28 stub terminal hooks, `web_create_terminal()`, `web_term_init()`, `syms_of_webterm()`
- `src/webfns.c` — 49-entry `web_frame_parm_handlers[]`, `syms_of_webfns()`

**Existing modifications to core files:**
- `src/termhooks.h` — `output_web` in enum, `web` in display_info union
- `src/frame.h` — `web_output` in output union, `FRAME_WEB_P()`, `FRAME_WINDOW_P()` extended
- `src/frame.c` — `Qweb` in `Fframep()` switch
- `src/dispextern.h` — conditional include of `webgui.h`, type aliases, `RGB_PIXEL_COLOR`
- `src/terminal.c` — `output_web` case
- `src/term.c` — `output_web` case
- `src/xdisp.c` — menu bar condition extended
- `src/emacs.c` — `syms_of_webterm()`, `syms_of_webfns()` calls
- `configure.ac` — `--with-web` option, `HAVE_WEB`, `WEB_OBJ`
- `src/Makefile.in` — `$(WEB_OBJ)` in base_obj

**Current defaults:** port 8080, char metrics 7x14, 24-bit color, scroll bars disabled.

## Architecture

```
┌──────────────┐   Unix socketpair    ┌──────────────────┐    WebSocket     ┌─────────┐
│    Emacs     │ ◄──────────────────► │  Display Proxy   │ ◄────────────► │ Browser │
│  (evaluator) │  draw cmds →         │  (web-display)   │  draw cmds →    │         │
│              │  ← input events      │                  │  ← input events │ Canvas  │
│  webterm.c   │                      │  Framebuffer     │                  │         │
│  webfns.c    │                      │  WebSocket srv   │                  │         │
└──────────────┘                      └──────────────────┘                  └─────────┘
```

**Why two processes:** When Emacs hangs in Lisp, the proxy keeps running — browser stays responsive. C-g from browser → proxy sends SIGINT to Emacs. Proxy maintains a framebuffer copy for instant state sync on reconnect/new client.

---

## Wire Protocol

Binary, little-endian. All messages prefixed with `[u8 opcode]`.

### Draw commands (Emacs → Proxy → Browser)
```
0x01 CLEAR_RECT    { x:u16, y:u16, w:u16, h:u16, color:u32 }           = 13 bytes
0x02 DRAW_GLYPHS   { x:u16, y:u16, fg:u32, bg:u32, flags:u8,           = 14 + len bytes
                     len:u16, utf8[] }
     flags: bit0=bold, bit1=italic, bit2=underline, bit3=strikethrough, bit4=box
0x03 DRAW_CURSOR   { x:u16, y:u16, w:u16, h:u16, type:u8, color:u32 }  = 14 bytes
0x04 SCROLL_RECT   { x:u16, y:u16, w:u16, h:u16, dy:i16 }              = 11 bytes
0x05 FILL_RECT     { x:u16, y:u16, w:u16, h:u16, color:u32 }           = 13 bytes
0x06 FLUSH         { }                                                   = 1 byte
0x07 FRAME_SIZE    { frame_id:u16, w:u16, h:u16, cw:u16, ch:u16 }      = 11 bytes
0x08 DRAW_IMAGE    { x:u16, y:u16, w:u16, h:u16, len:u32, png[] }      = 13 + len bytes
0x09 HEARTBEAT     { timestamp_ms:u64 }                                  = 9 bytes
```

### Input events (Browser → Proxy → Emacs)
```
0x80 KEY_EVENT     { keycode:u32, modifiers:u8, utf32_char:u32 }        = 10 bytes
0x81 MOUSE_BUTTON  { x:u16, y:u16, button:u8, pressed:u8, mods:u8 }    = 8 bytes
0x82 MOUSE_MOVE    { x:u16, y:u16, mods:u8 }                            = 6 bytes
0x83 SCROLL        { x:u16, y:u16, dx:i16, dy:i16, mods:u8 }           = 10 bytes
0x84 RESIZE        { pixel_w:u16, pixel_h:u16 }                         = 5 bytes
0x85 FOCUS         { gained:u8 }                                         = 2 bytes
0x86 CLIPBOARD     { dir:u8, len:u32, utf8[] }                          = 6 + len bytes
0x87 INTERRUPT     { }                                                   = 1 byte  (→ SIGINT)
0xF1 FONT_METRICS  { char_w:u16, char_h:u16, ascent:u16, descent:u16 } = 9 bytes
```

---

## Phase 2: Display Proxy — Standalone WebSocket Server

**Goal:** Build `web-display/emacs-web-display` as a standalone C program. Test independently — no Emacs integration yet.

### Step 2.1: Create directory structure + Makefile

```
web-display/
  main.c          — entry point, kqueue/epoll event loop
  websocket.c/h   — RFC 6455 WebSocket server (handshake, frame parse/build)
  protocol.c/h    — wire protocol encode/decode for all opcodes
  framebuffer.c/h — cell grid: struct cell { u32 codepoint; u32 fg, bg; u8 flags; }
  Makefile         — build emacs-web-display binary
```

### Step 2.2: WebSocket server (`websocket.c`)

- `ws_server_create(port)` — bind, listen
- `ws_server_accept(srv)` — accept client, perform HTTP upgrade handshake
- `ws_frame_read(client_fd, buf, len)` — parse WebSocket frame (opcode, payload, unmask)
- `ws_frame_write(client_fd, opcode, data, len)` — build and send WebSocket frame
- Support binary frames only (opcode 0x02), plus ping/pong/close
- Non-blocking I/O throughout

### Step 2.3: Event loop (`main.c`)

- `kqueue` on macOS / `epoll` on Linux
- Monitor: server socket (accept), client sockets (read), stdin (Emacs draw commands later)
- Fan-out: any draw command received → forward to all connected clients
- Any input event from any client → write to stdout (for Emacs later)

### Step 2.4: Framebuffer (`framebuffer.c`)

- `fb_create(cols, rows)` — allocate cell grid
- `fb_clear_rect(fb, x, y, w, h, color)` — update cells
- `fb_write_glyphs(fb, x, y, chars, len, fg, bg, flags)` — update cells
- `fb_replay(fb, client_fd)` — send entire framebuffer as DRAW_GLYPHS messages (for new client sync)

### Step 2.5: Protocol codec (`protocol.c`)

- `proto_encode_*(buf, ...)` — serialize each draw command to buffer
- `proto_decode(buf, len, msg*)` — parse a message, return opcode + decoded fields
- Length-prefix each message on the Emacs↔Proxy socket (WebSocket already has framing)

### Validation 2
```bash
cd web-display && make
# Standalone test:
echo -ne '\x05\x00\x00\x00\x00\x64\x00\x32\x00\xff\x00\x00\x00' | ./emacs-web-display --port 8080
# ^ sends a FILL_RECT — proxy should forward to any connected browser client
# (also test with a small harness that connects via WebSocket and logs messages)
```

---

## Phase 3: Browser Client — Canvas Renderer + Input Capture

**Goal:** Browser connects to Display Proxy, renders draw commands on Canvas, sends input events back. Tested with fake draw commands from proxy stdin.

### Step 3.1: Create `web-client/index.html`

- Full-viewport `<canvas>` element
- Load modules: `renderer.js`, `input.js`, `protocol.js`
- Connect to `ws://localhost:8080`
- Status bar showing connection state

### Step 3.2: Protocol decoder (`web-client/protocol.js`)

- `decodeMessage(DataView, offset)` → `{ opcode, ...fields }`
- `encodeKeyEvent(keycode, mods, char)` → `ArrayBuffer`
- `encodeMouseButton(x, y, button, pressed, mods)` → `ArrayBuffer`
- `encodeResize(w, h)` → `ArrayBuffer`
- etc. for all input event opcodes

### Step 3.3: Canvas renderer (`web-client/renderer.js`)

- Monospace font (configurable, default `16px monospace`)
- On each binary WebSocket message, decode and dispatch:
  - `CLEAR_RECT` → `ctx.fillStyle = color; ctx.fillRect(x, y, w, h)`
  - `DRAW_GLYPHS` → `ctx.fillStyle = bg; ctx.fillRect(...)` then `ctx.fillStyle = fg; ctx.fillText(text, x, y+ascent)` with bold/italic font variants
  - `DRAW_CURSOR` → semi-transparent overlay rect or box outline
  - `SCROLL_RECT` → `ctx.drawImage(canvas, sx, sy, sw, sh, dx, dy, dw, dh)` then clear exposed area
  - `FILL_RECT` → `ctx.fillStyle = color; ctx.fillRect(...)`
  - `FLUSH` → (noop in immediate mode, or commit in double-buffer mode)
  - `FRAME_SIZE` → resize canvas to match

### Step 3.4: Input capture (`web-client/input.js`)

- `document.addEventListener('keydown', ...)` → encode KEY_EVENT, send over WebSocket
- Map modifiers: `ctrlKey→ctrl`, `altKey→meta`, `shiftKey→shift`, `metaKey→super`
- `event.preventDefault()` to intercept browser shortcuts (Ctrl+W, Ctrl+T, Ctrl+N, etc.)
- Ctrl+G → send INTERRUPT (0x87) instead of KEY_EVENT
- `mousedown`/`mouseup` → MOUSE_BUTTON
- `mousemove` → MOUSE_MOVE (throttled ~60fps)
- `wheel` → SCROLL
- `ResizeObserver` on canvas → RESIZE
- `focus`/`blur` → FOCUS

### Validation 3
```bash
cd web-display && make
./emacs-web-display --port 8080 --test-pattern &
# test-pattern flag: proxy sends fake draw commands (colored rectangles, "Hello Emacs" text)

cd ../web-client && python3 -m http.server 3000
open http://localhost:3000
# Should see colored rects and "Hello Emacs" rendered on canvas
# Type → KEY_EVENT messages logged in proxy stdout
# Click → MOUSE_BUTTON messages logged
# Resize browser → RESIZE logged
```

---

## Phase 4: Connect Emacs to Display Proxy

**Goal:** `web_term_init()` forks the proxy, Emacs sends draw commands through it, text appears in browser.

### Step 4.1: Fork proxy from `web_term_init()` (modify `src/webterm.c`)

```c
// In web_term_init():
int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
pid_t pid = vfork();
if (pid == 0) {
    close(sv[0]);
    char fd_str[16]; snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
    execl("libexec/emacs-web-display", "emacs-web-display",
          "--emacs-fd", fd_str, "--port", port_str, NULL);
    _exit(1);
}
close(sv[1]);
dpyinfo->proxy_fd = sv[0];
dpyinfo->proxy_pid = pid;
```

Register `proxy_fd` with Emacs's event loop:
```c
add_read_fd(dpyinfo->proxy_fd, web_proxy_input_ready, dpyinfo);
```

### Step 4.2: Implement draw command serialization

Modify existing stubs in `webterm.c` to actually serialize and write to `proxy_fd`:

**`web_clear_frame_area(f, x, y, w, h)`** → write CLEAR_RECT (0x01)
**`web_draw_glyph_string(s)`** → extract from `struct glyph_string *s`:
  - chars: iterate `s->char2b[0..s->nchars]` or walk glyphs
  - position: `s->x`, `s->y`
  - colors: `s->face->foreground`, `s->face->background`
  - flags: `s->face->lface` weight/slant + underline/strikethrough
  - Write DRAW_GLYPHS (0x02)
**`web_draw_window_cursor(w, row, x, y, type, width, on_p, active_p)`** → write DRAW_CURSOR (0x03)
**`web_scroll_run(w, run)`** → write SCROLL_RECT (0x04)
**`web_flush_display(f)`** → write FLUSH (0x06), then `fsync` or let pipe buffer handle it
**`web_draw_vertical_window_border(w, x, y0, y1)`** → write FILL_RECT (0x05)
**`web_draw_window_divider(w, x0, x1, y0, y1)`** → write FILL_RECT (0x05)

Use a write buffer to batch small messages between FLUSHes.

### Step 4.3: Implement `web_read_socket()` — input from proxy

Non-blocking read from `proxy_fd`:
- Parse messages by opcode
- KEY_EVENT → `struct input_event` with:
  - `kind = ASCII_KEYSTROKE_EVENT` or `NON_ASCII_KEYSTROKE_EVENT`
  - `code = utf32_char`
  - `modifiers` mapped from protocol modifiers
  - Call `kbd_buffer_store_event(&ev)`
- MOUSE_BUTTON → `kind = MOUSE_CLICK_EVENT`, set `x`, `y`, `frame_or_window`
- MOUSE_MOVE → update `dpyinfo->last_mouse_x/y`
- SCROLL → `kind = WHEEL_EVENT`
- RESIZE → call `change_frame_size()`
- FOCUS → update `dpyinfo->x_focus_frame`
- FONT_METRICS → update `dpyinfo->default_char_width/height`

### Step 4.4: Hardcode font metrics

`web_default_font_parameter()`:
- Set frame font to a dummy font object with char_width=7, char_height=14 (already in defaults)
- This gets replaced with real browser metrics in Phase 7

### Step 4.5: Frame creation (`webfns.c`)

Implement `Fweb_create_frame()`:
- Allocate `struct web_output`, set `output_method = output_web`
- Initialize colors, char dimensions from `dpyinfo` defaults
- Send FRAME_SIZE to proxy
- This function is modeled on `Fpgtk_create_frame()` in `pgtkfns.c`

### Validation 4
```bash
cd ~/Documents/code/emacs
make -j$(nproc)
cd web-display && make && cd ..

./src/emacs -Q --web-display
# Open http://localhost:8080 in browser
# Should see *scratch* buffer text on canvas
# Mode line and minibuffer visible
# Type characters → they appear in buffer
# C-x C-f → minibuffer prompts for file
# C-g → cancels
# Arrow keys → cursor moves
```

---

## Phase 5: Mouse, Resize, Focus

**Goal:** Full mouse interaction, dynamic resize, focus tracking.

### Step 5.1: Mouse click → point movement

- MOUSE_BUTTON events already parsed in 4.3
- Ensure `x`, `y` are pixel coordinates relative to frame
- Emacs internally converts pixel → buffer position via `window_from_coordinates()`

### Step 5.2: Scroll wheel

- SCROLL events → `WHEEL_EVENT` kind
- Set `arg` with scroll delta

### Step 5.3: Resize

- RESIZE event → `change_frame_size(f, new_cols, new_rows, false, true, false)`
- Recalculate cols/rows from pixel dimensions and char cell size

### Step 5.4: Focus tracking

- FOCUS event → update `dpyinfo->x_focus_frame`
- Call `x_new_focus_frame()` equivalent

### Validation 5
```bash
make -j$(nproc)
# Click to position cursor ✓
# Drag-select text → region highlights ✓
# Scroll wheel → buffer scrolls ✓
# Resize browser → text reflows ✓
```

---

## Phase 6: Proper Colors and Face Rendering

**Goal:** Syntax highlighting, styled mode line.

### Step 6.1: Color system

- `web_defined_color()` → parse X11 named colors ("white", "black", "red", etc.)
- Use Emacs's existing color name database (reuse patterns from `xfaces.c`)
- Return RGB packed as `(r << 16) | (g << 8) | b`

### Step 6.2: Full face extraction in `web_draw_glyph_string()`

Already partially in Step 4.2. Ensure:
- Bold: check `FONT_WEIGHT_NAME_NUMERIC(s->face->lface[LFACE_WEIGHT_INDEX]) > 100`
- Italic: check slant
- Underline: `s->face->underline`
- Strikethrough: `s->face->strike_through_p`

### Step 6.3: Canvas renderer styling

Already partially in Step 3.3. Ensure font string includes bold/italic:
```js
ctx.font = `${bold ? 'bold ' : ''}${italic ? 'italic ' : ''}16px monospace`;
```

### Validation 6
```bash
make -j$(nproc)
# Open .c file → syntax highlighting ✓
# Mode line has distinct background color ✓
# Region highlight works ✓
# M-x list-faces-display → colors correct ✓
```

---

## Phase 7: Font Driver (`webfont.c`)

**Goal:** Proper font objects, accurate metrics from browser.

### Step 7.1: Create `src/webfont.c`

Implement `struct font_driver web_font_driver`:
- `list()` — return entities for monospace families (Courier, Menlo, DejaVu Sans Mono)
- `match()` — best match for spec
- `open_font()` — allocate `struct font`, fill metrics from `dpyinfo->default_char_width/height`
- `close_font()` — free
- `has_char()` → 1
- `encode_char()` → identity
- `text_extents()` → width = nchars * char_width

### Step 7.2: Register in frame creation

`register_font_driver(&web_font_driver, f)` in `Fweb_create_frame()`

### Step 7.3: Browser-side font measurement

On connect, browser measures and sends FONT_METRICS (0xF1):
```js
ctx.font = '16px monospace';
const m = ctx.measureText('M');
// send: char_w, line_height, ascent, descent
```

### Step 7.4: Add to build

Add `webfont.o` to `WEB_OBJ` in `configure.ac`.

### Validation 7
```bash
make -j$(nproc)
# Text properly aligned, no character overlap ✓
# M-x describe-font → shows web font backend ✓
# (set-face-attribute 'default nil :height 200) → larger text ✓
```

---

## Phase 8: Images, Fringes, Menus, Clipboard

**Goal:** Full GUI parity.

### Step 8.1: Image rendering
- `web_draw_glyph_string()` handles `IMAGE_GLYPH` type
- Encode as PNG in DRAW_IMAGE message
- Browser: `createImageBitmap(blob)` → `ctx.drawImage()`

### Step 8.2: Fringe bitmaps
- `web_draw_fringe_bitmap()` → serialize bitmap data or send bitmap ID
- Browser has standard fringe bitmaps pre-defined

### Step 8.3: Menu bar (`src/webmenu.c`)
- Serialize menu tree as JSON
- Browser renders as HTML overlay
- Selection → input event

### Step 8.4: Clipboard (`src/webselect.c`)
- CLIPBOARD protocol messages
- Browser ↔ `navigator.clipboard` API

### Step 8.5: Add to build
- Add `webmenu.o webselect.o` to `WEB_OBJ`

### Validation 8
```bash
make -j$(nproc)
# View image file → renders ✓
# Fringes visible ✓
# Menu bar works ✓
# Copy/paste with system clipboard ✓
```

---

## Phase 9: Resilience, Multi-client, Polish

### 9.1: Framebuffer replay on new client connect
### 9.2: Heartbeat → "Emacs is busy" browser overlay
### 9.3: Auto-reconnect with exponential backoff
### 9.4: Multiple simultaneous browser clients (fan-out)
### 9.5: TLS/WSS + token auth
### 9.6: Performance: dirty-rect tracking, batch sends, compression

---

## File Summary

### Already exists (Phase 1 — DONE)
| File | Status |
|------|--------|
| `src/webterm.h` | Created — structs, macros |
| `src/webgui.h` | Created — X-compat types |
| `src/webterm.c` | Created — stubs, needs real implementation |
| `src/webfns.c` | Created — stubs, needs frame creation |
| Core file mods | Done — termhooks.h, frame.h, emacs.c, configure.ac, Makefile.in, etc. |

### To create
| File | Phase | Purpose |
|------|-------|---------|
| `web-display/main.c` | 2 | Proxy entry point, event loop |
| `web-display/websocket.c/h` | 2 | RFC 6455 WebSocket server |
| `web-display/protocol.c/h` | 2 | Wire protocol codec |
| `web-display/framebuffer.c/h` | 2 | In-memory display state |
| `web-display/Makefile` | 2 | Build proxy |
| `web-client/index.html` | 3 | Browser entry point |
| `web-client/renderer.js` | 3 | Canvas 2D rendering |
| `web-client/input.js` | 3 | Keyboard/mouse capture |
| `web-client/protocol.js` | 3 | Binary message codec |
| `src/webfont.c` | 7 | Font driver |
| `src/webmenu.c` | 8 | Menu support |
| `src/webselect.c` | 8 | Clipboard/selection |
