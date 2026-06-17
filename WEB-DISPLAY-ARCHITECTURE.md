# Emacs Web Display Backend — Architecture

A browser-based display backend for GNU Emacs. Emacs renders its frame as
structured JSON (windows, lines, styled text runs) and pipes it to a
WebSocket proxy, which forwards it to a browser that renders with Preact
and the DOM. Input flows the other direction: browser captures keyboard,
mouse, and scroll events, encodes them as JSON, and sends them back through
the proxy to Emacs.

```
┌──────────────────────────────────────────────────────────────────────┐
│                        ARCHITECTURE                                  │
│                                                                      │
│  ┌──────────┐   Unix socket   ┌──────────────┐  WebSocket  ┌──────┐ │
│  │  Emacs   │ ──── NDJSON ──> │  Proxy       │ ── JSON ──> │ Web  │ │
│  │  (C)     │ <── NDJSON ──── │  (C binary)  │ <── JSON ── │Client│ │
│  └──────────┘    socketpair   └──────────────┘   TCP:8080   └──────┘ │
│   src/webterm.c                web-display/       web-client/        │
│   src/webterm.h                  main.c             app.js           │
│   src/webgui.c                   websocket.c        state.js         │
│   lisp/term/web-win.el           websocket.h        input.js         │
│                                  Makefile            components/     │
│                                                      emacs.css       │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Table of Contents

1. [Design Principles](#1-design-principles)
2. [System Overview](#2-system-overview)
3. [Tier 1: Emacs Core](#3-tier-1-emacs-core)
4. [Tier 2: WebSocket Proxy](#4-tier-2-websocket-proxy)
5. [Tier 3: Browser Client](#5-tier-3-browser-client)
6. [JSON Wire Protocol](#6-json-wire-protocol)
7. [Data Flow Walkthrough](#7-data-flow-walkthrough)
8. [Startup Sequence](#8-startup-sequence)
9. [Building and Running](#9-building-and-running)
10. [Debug REPL](#10-debug-repl)
11. [File Index](#11-file-index)
12. [Future Work](#12-future-work)

---

## 1. Design Principles

**Semantic over pixel.** Emacs has rich semantic data — windows, lines,
styled glyph runs with named faces. Rather than flattening this into pixel
coordinates and binary draw commands, the web backend preserves the
structure and sends it as JSON. The browser renders using its native text
engine, gaining text selection, accessibility, zoom, and font rendering for
free.

**Character-cell grid.** Positions and dimensions use character-cell units
(`cols` × `rows`), not pixels. The browser maps these to CSS `ch` and `em`
units. On resize, the browser measures its own font metrics and sends
`cols`/`rows` back to Emacs.

**Thin proxy.** The proxy is a dumb pipe — it does not parse, interpret, or
transform the JSON. It reads newline-delimited JSON from Emacs over a Unix
socket, broadcasts each line as a WebSocket text frame, and forwards
browser messages back. This makes it trivial to maintain and debug.

**Incremental updates.** Only dirty lines are sent each frame. The browser
merges them into its state. Scroll operations send a delta rather than
redrawing every line.

**No build step in the browser.** The client uses Preact via an ES module
import map from a CDN. No webpack, no bundler, no node_modules. Just open
`index.html`.

---

## 2. System Overview

The system has three tiers connected by two communication channels:

| Tier | Language | Role |
|------|----------|------|
| Emacs Core | C + Elisp | Implements the Emacs Redisplay Interface (RIF). Accumulates semantic redisplay data into structs during a redisplay cycle, serializes to NDJSON at flush, parses input JSON from the browser. |
| WebSocket Proxy | C | Bridges a Unix socket (to Emacs) and WebSocket connections (to browsers). Handles RFC 6455 handshake, framing, masking. Manages multiple browser clients. |
| Browser Client | JS + CSS | Connects via WebSocket, maintains frame state (faces, windows, lines), renders with Preact components, captures input events. |

**IPC channel (Emacs ↔ Proxy):** A `socketpair(AF_UNIX, SOCK_STREAM)`
created at startup. Emacs writes NDJSON lines; the proxy reads them
line-by-line and broadcasts. The proxy writes browser input back; Emacs
reads and dispatches.

**Network channel (Proxy ↔ Browser):** TCP WebSocket on port 8080. Text
frames carry JSON in both directions. Multiple browsers can connect
simultaneously; all receive the same broadcast.

---

## 3. Tier 1: Emacs Core

### 3.1 Files

| File | Lines | Role |
|------|-------|------|
| `src/webterm.h` | ~330 | Data structures: `web_display_info`, `web_output`, `json_frame_state` |
| `src/webterm.c` | ~2140 | RIF implementation, JSON serialization, input parsing, proxy lifecycle |
| `src/webgui.c` | ~860 | Frame/window creation, color allocation, font handling, X11-compatible API shim |
| `src/webgui.h` | ~100 | Declarations for webgui.c functions |
| `lisp/term/web-win.el` | 69 | Elisp glue: registers `web` window system, starts 60Hz timer |

### 3.2 Key Data Structures

#### `struct web_display_info` (webterm.h)

The central display state. One instance exists for the lifetime of the
session. Key fields:

```c
struct web_display_info {
  /* IPC with proxy */
  int proxy_fd;               /* Unix socket to proxy process */
  pid_t proxy_pid;            /* PID of proxy child process */
  int port;                   /* WebSocket port (default 8080) */

  /* I/O buffers */
  uint8_t *write_buf;         /* NDJSON output buffer (64KB initial) */
  int write_buf_len, write_buf_size;
  uint8_t *read_buf;          /* NDJSON input buffer (64KB) */
  int read_buf_len, read_buf_size;

  /* Font metrics (set by browser's font_metrics message) */
  int default_char_width;     /* pixels per character */
  int default_char_height;    /* pixels per line */

  /* Redisplay accumulator */
  struct json_frame_state json_state;

  /* Display properties */
  int n_planes;               /* color depth (24) */
  double resx, resy;          /* DPI (72) */
  struct frame *x_focus_frame;
  struct terminal *terminal;
  /* ... color_map, kboard, linked-list pointers ... */
};
```

#### `struct json_frame_state` (webterm.h)

Hierarchical accumulator filled during a single redisplay cycle, then
serialized to JSON at flush and reset. This is the core of the semantic
approach — instead of emitting draw commands immediately, we collect
structured data:

```
json_frame_state
├── windows[16]                   ← up to 16 Emacs windows
│   ├── id, x, y, w, h           ← window identity and geometry (char cells)
│   ├── lines[256]                ← up to 256 lines per window
│   │   ├── row_index             ← which row in the window
│   │   ├── mode_line_p           ← is this the mode line?
│   │   ├── continued_p           ← line continuation marker
│   │   └── runs[256]             ← up to 256 styled text runs per line
│   │       ├── face_id           ← Emacs face ID (or synthetic)
│   │       └── text[4096]        ← UTF-8 text content
│   ├── cursor {row, col, type, active}
│   └── has_cursor
├── face_ids[512]                 ← face IDs referenced this cycle
├── scrolls[16]                   ← scroll events {window_id, delta_rows}
├── clear_pending, clear_bg       ← full-frame clear request
├── current_window                ← pointer during accumulation
└── current_line, current_line_row
```

### 3.3 RIF Functions (Redisplay Interface)

Emacs calls these function pointers during its redisplay cycle. The web
backend's implementations accumulate data into `json_frame_state`:

| RIF Hook | Implementation | What It Does |
|----------|---------------|--------------|
| `update_window_begin` | `web_update_window_begin` | Allocates a `json_window` slot, sets geometry from `WINDOW_LEFT_EDGE_COL`, `WINDOW_TOP_EDGE_LINE`, `WINDOW_TOTAL_COLS`, `WINDOW_TOTAL_LINES` |
| `update_window_end` | `web_update_window_end` | Clears `current_window` pointer |
| `draw_glyph_string` | `web_draw_glyph_string` | Main workhorse. Extracts UTF-8 text from glyph strings, determines face ID (with synthetic bits for cursor/inverse), appends as `json_run` to current line. Merges consecutive runs with same face. Handles CHAR, COMPOSITE, STRETCH, IMAGE, GLYPHLESS glyph types. |
| `after_update_window_line` | `web_after_update_window_line` | Sets line metadata: `mode_line_p`, `continued_p`, `truncated_on_left/right_p` |
| `scroll_run` | `web_scroll_run` | Records `{window_id, delta_rows}` for scroll optimization |
| `draw_window_cursor` | `web_draw_window_cursor` | Records cursor position (row, col), type (box=0, hollow=1, bar=2, hbar=3), and active flag. Calls `draw_phys_cursor_glyph` to render the character under cursor with cursor face. |
| `flush_display` | `web_flush_display` | **Serializes the entire `json_frame_state` to NDJSON and writes to proxy_fd.** This is where all the JSON output happens. Emits `clear_frame`, `scroll`, `frame_update` (with faces dict + windows array), and `heartbeat` messages. Resets state after flush. |
| `clear_frame` | `web_clear_frame` | Sets `clear_pending = true`, records background color |
| `draw_fringe_bitmap` | no-op | Browser uses CSS margins instead |
| `draw_vertical_window_border` | no-op | Browser renders via CSS borders |
| `draw_window_divider` | no-op | Browser renders via CSS |
| `clear_frame_area` | no-op | Implicit in browser rendering |

### 3.4 Face ID Encoding

Face IDs carry semantic bits:

| Bits | Meaning |
|------|---------|
| `face_id & 0xFFFFF` | Real Emacs face ID (looked up via `FACE_FROM_ID`) |
| `face_id \| 0x100000` | Cursor highlight — uses `cursor_foreground_color` and `FRAME_CURSOR_COLOR` |
| `face_id \| 0x200000` | Inverse video / mouse face — swaps fg and bg |

The faces dictionary in `frame_update` maps these IDs to resolved
`{fg, bg, bold, italic, underline, strike, box}` properties.

### 3.5 Input Parsing — `web_read_socket`

Reads NDJSON lines from `proxy_fd` and creates Emacs input events:

```
Browser JSON          →  Emacs Event
─────────────────────────────────────────
"key"                 →  KEY_EVENT / NON_ASCII_KEYSTROKE_EVENT
"mouse_down"          →  MOUSE_CLICK_EVENT (button_down)
"mouse_up"            →  MOUSE_CLICK_EVENT (button_up)
"scroll"              →  WHEEL_EVENT (scroll-up/down/left/right)
"resize"              →  change_frame_size(f, cols, rows, ...)
"focus"               →  FOCUS_IN_EVENT / FOCUS_OUT_EVENT
"font_metrics"        →  Updates FRAME_COLUMN_WIDTH, FRAME_LINE_HEIGHT
"clipboard"           →  Sets Vweb_clipboard
"interrupt"           →  Emacs_Quit
"request_redraw"      →  SET_FRAME_GARBAGED → full redisplay
```

Modifier bits are decoded from the wire format to Emacs modifier masks:

```
Wire (browser)        →  Emacs
WIRE_MOD_SHIFT (1<<0) →  shift_modifier
WIRE_MOD_CTRL  (1<<2) →  ctrl_modifier
WIRE_MOD_META  (1<<3) →  meta_modifier
WIRE_MOD_SUPER (1<<4) →  super_modifier
```

The parser uses lightweight string-matching helpers (`json_find_key`,
`json_extract_int`, `json_extract_bool`, `json_extract_string`,
`json_type_is`) to avoid needing a full JSON parser library.

### 3.6 Write Buffer and WR_LIT Macro

All JSON output goes through a growable write buffer:

```c
web_write_ensure(dpyinfo, need);  /* grow if needed */
web_write_str(dpyinfo, s, len);   /* append bytes */
web_write_printf(dpyinfo, fmt, ...); /* formatted append */
web_write_flush(dpyinfo);         /* non-blocking write to proxy_fd */
```

To eliminate manual byte-counting bugs (which caused a real blank-screen
bug during development), string literals use a compile-time macro:

```c
#define WR_LIT(dp, s) web_write_str(dp, s, sizeof(s) - 1)

// Instead of error-prone:
//   web_write_str(dpyinfo, "{\"type\":\"frame_update\"", 22);
// We write:
//   WR_LIT(dpyinfo, "{\"type\":\"frame_update\"");
```

`sizeof("literal") - 1` is computed at compile time and is always correct.

### 3.7 Elisp Integration — `web-win.el`

Loaded when Emacs starts with `--web`. Does four things:

1. Registers `"web"` in `display-format-alist` so Emacs routes display
   calls to the web backend.
2. Implements `window-system-initialization` — calls `x-open-connection`
   which triggers `web_term_init()` in C.
3. Adds an `after-init-hook` to start the 60Hz redisplay timer
   (`web--start-redisplay-timer`) after initialization completes.
4. Implements the selection backend for the `web` window-system —
   `gui-backend-{set,get}-selection` and `selection-{owner,exists}-p`
   `cl-defmethod`s with `&context (window-system web)`. These bridge
   the standard `gui-select-text` / `gui-selection-value` machinery to
   `web-set-clipboard` (out) and `web-clipboard-text` (in); see the
   `clipboard` wire message in §6.2. Without them M-w reached only the
   kill ring. Only CLIPBOARD is mirrored to the browser; PRIMARY stays
   local so `select-active-regions` doesn't clobber the OS clipboard.

### 3.8 60Hz Redisplay Timer

A periodic `atimer` fires every ~16ms (`web_redisplay_timer_callback`):

1. Sends a `heartbeat` JSON message so the browser knows Emacs is alive.
2. Calls `Fredisplay(Qt)` to trigger a redisplay cycle if one is needed.

This ensures the display stays responsive even when Emacs is idle (no
keyboard input to trigger redisplay). The timer is started from Elisp
after init to avoid interfering with byte-compilation during startup.

---

## 4. Tier 2: WebSocket Proxy

### 4.1 Files

| File | Role |
|------|------|
| `web-display/main.c` | Event loop, Emacs↔browser message forwarding |
| `web-display/websocket.c` | RFC 6455 WebSocket implementation (handshake, framing, masking) |
| `web-display/websocket.h` | Public API and data structures |
| `web-display/Makefile` | Build: platform detection (kqueue/epoll), compiles to `emacs-web-display` |

### 4.2 Architecture

The proxy is deliberately simple — a **JSON gateway** that does not
interpret messages:

```
                     ┌─────────────────────────────┐
Emacs (pipe) ──────> │  emacs_buf (256KB)           │
                     │  Line-buffer NDJSON          │
                     │  For each \n-terminated line: │
                     │    ws_broadcast_text()    ────────> Browser A
                     │                              │────> Browser B
                     │                              │────> Browser C
                     │                              │
Browser A ────────>  │  on_client_message()          │
Browser B ────────>  │    "interrupt" → kill(SIGINT) │
Browser C ────────>  │    else → write() to Emacs ───────> Emacs (pipe)
                     └─────────────────────────────┘
```

### 4.3 Event Loop

Platform-specific I/O multiplexing:

- **macOS:** `kqueue` with `kevent()`. Monitors the listen socket, the
  Emacs fd, and each client fd. Client fds are registered/deregistered
  dynamically as connections come and go.
- **Linux:** `epoll` with `epoll_wait()`. Same logic, different API.

Both variants handle the same events:
1. **Listen socket readable** → `ws_server_accept()` → register new client
2. **Emacs fd readable** → `read()` into `emacs_buf` → `process_emacs_data()` → broadcast complete lines
3. **Client fd readable** → `ws_client_read()` → on complete message, call `on_client_message()`

### 4.4 Client Lifecycle

1. TCP accept → client enters `WS_STATE_HTTP`
2. Client sends HTTP Upgrade request → proxy validates, computes
   `Sec-WebSocket-Accept` via SHA-1 + Base64, sends 101 response →
   client enters `WS_STATE_OPEN`
3. On successful upgrade, proxy sends `{"type":"request_redraw"}` to
   Emacs so the new client gets a full frame
4. Client sends/receives WebSocket text frames
5. On disconnect or error → `ws_remove_client()`, deregister from
   kqueue/epoll

### 4.5 WebSocket Implementation

The WebSocket code is self-contained with no external dependencies:

- **SHA-1**: Inline implementation (~60 lines) for the handshake
- **Base64**: Inline encoder for the accept key
- **Framing**: Handles variable-length payloads (7-bit, 16-bit, 64-bit
  length encoding), client→server masking (XOR with 4-byte key),
  continuation frames
- **Opcodes**: Text (0x1), Binary (0x2), Close (0x8), Ping (0x9),
  Pong (0xA)

Key functions:

```c
ws_server_create(srv, port)           // bind + listen
ws_server_accept(srv)                 // accept connection
ws_client_read(srv, idx, cb, ud)      // parse frames, invoke callback
ws_send_text(srv, idx, data, len)     // send text frame to one client
ws_broadcast_text(srv, data, len)     // send text frame to all clients
ws_remove_client(srv, idx)            // close + cleanup
ws_server_destroy(srv)                // shutdown
```

### 4.6 Command Line

```
emacs-web-display [--port PORT] [--emacs-fd FD]

  --port PORT       WebSocket listen port (default: 8080)
  --emacs-fd FD     File descriptor for Emacs IPC (default: stdin/stdout)
```

Normally launched by Emacs via `fork()`+`exec()` with `--emacs-fd` set to
the socketpair fd.

---

## 5. Tier 3: Browser Client

### 5.1 Files

| File | Role |
|------|------|
| `web-client/index.html` | Entry point. Preact import map (CDN), mounts `<div id="app">` |
| `web-client/app.js` | Root Preact component. WebSocket lifecycle, font measurement, resize handling |
| `web-client/state.js` | `FrameState` class — stores faces, windows, lines; dispatches updates |
| `web-client/input.js` | `InputHandler` class — captures keyboard, mouse, scroll, focus; encodes JSON |
| `web-client/emacs.css` | Monospace grid layout, cursor animations, status bar |
| `web-client/components/frame.js` | Renders all windows sorted by position |
| `web-client/components/window.js` | Renders lines 0..h for a single window |
| `web-client/components/line.js` | Renders styled text runs as `<span>` elements, handles cursor splitting |

### 5.2 State Management — `FrameState`

Central state store. Holds the current view of the Emacs frame:

```
FrameState
├── faces: Map<face_id → {fg, bg, bold, italic, underline, strike, box}>
├── windows: Map<window_id → WindowState>
│   └── WindowState
│       ├── id, x, y, w, h              (character-cell geometry)
│       ├── lines: Map<row → LineData>
│       │   └── LineData
│       │       ├── row, mode_line, continued
│       │       └── runs: [{face_id, text}, ...]
│       └── cursor: {row, col, type, active}
├── defaultFg, defaultBg                  (frame default colors)
├── cols, rows                            (frame dimensions)
└── _listeners: [callback, ...]           (onChange notification)
```

Message dispatch:

```javascript
dispatch(msg) {
  switch (msg.type) {
    case 'frame_update': this._applyFrameUpdate(msg); break;
    case 'scroll':       this._applyScroll(msg);      break;
    case 'clear_frame':  this._applyClear(msg);        break;
    case 'frame_size':   this._applyFrameSize(msg);    break;
  }
  this._notify();  // triggers Preact re-render
}
```

Updates are **incremental**: `_applyFrameUpdate` merges only the faces and
lines present in the message, leaving unchanged data intact.

### 5.3 Rendering Pipeline

```
WebSocket message
  → state.dispatch(msg)         // update FrameState
  → state._notify()             // call listeners
  → setForceUpdate(n+1)         // trigger Preact re-render
  → requestAnimationFrame       // batch to next frame
  → renderFrame(state)          // top-level render function

renderFrame(state)
  → for each window in state.windows (sorted by y,x):
      renderWindow(state, window)
        → for row 0..window.h:
            renderLine(state, window, row)
              → for each run in line.runs:
                  <span style="color:{fg}; background:{bg}; ...">
                    {run.text}
                  </span>
              → split run at cursor.col to insert cursor <span>
```

### 5.4 Cursor Rendering

The line component handles cursor display:

1. Finds the run containing `cursor.col`
2. Splits the text at that column
3. Inserts a `<span>` with a CSS cursor class:
   - `cursor-box` (type 0): inverted colors, blink animation
   - `cursor-hollow` (type 1): outline border, blink
   - `cursor-bar` (type 2): left border, blink
   - `cursor-hbar` (type 3): bottom border, blink
   - `cursor-inactive`: hollow, no blink (unfocused window)
4. If cursor is past the end of all runs, renders a space at the cursor
   position

### 5.5 Input Handling — `InputHandler`

Captures DOM events and sends JSON to Emacs:

**Keyboard:**
- `keydown` → maps `e.key` to keycode (special keys use X11 keycodes),
  handles Ctrl+letter as control characters (0x01–0x1A)
- `paste` → sends clipboard text

**Mouse:**
- `mousedown`/`mouseup` → pixel position relative to container, button
  number, modifiers
- `mousemove` → throttled to 60fps (16ms), same format

**Scroll:**
- `wheel` → dx/dy deltas clamped to int16 range

**Focus:**
- `window.focus`/`blur` → gained true/false

**Special key mapping (matching X11 keycodes):**
```
Backspace → 0xFF08    Home     → 0xFF50    F1  → 0xFFBE
Tab       → 0xFF09    End      → 0xFF57    F2  → 0xFFBF
Enter     → 0xFF0D    PageUp   → 0xFF55    ...
Escape    → 0xFF1B    PageDown → 0xFF56    F12 → 0xFFC9
Delete    → 0xFFFF    Arrows   → 0xFF51-54
```

### 5.6 WebSocket Connection

Managed in `app.js`:

1. Connect to `ws://localhost:8080`
2. On open:
   - Measure font metrics (create invisible `<span>` with "M")
   - Send `font_metrics` to Emacs
   - Send `resize` with computed `cols`/`rows`
   - Send `focus` gained
   - Create `InputHandler`
3. On message: parse JSON, dispatch to `FrameState`
4. On close/error: exponential backoff reconnect (1s → 2s → 4s → ... → 10s cap)

### 5.7 Resize Handling

A `ResizeObserver` monitors the frame container:

1. Container size changes
2. Compute `cols = floor(width / charWidth)`,
   `rows = floor(height / charHeight)`
3. Send `{"type":"resize","cols":N,"rows":N}` to Emacs
4. Emacs calls `change_frame_size()` → triggers full redisplay

### 5.8 CSS Layout

Monospace character-cell grid using CSS units:

```css
.emacs-frame   { font: 16px monospace; line-height: 1.2; }
.emacs-window  { position: absolute;
                 left: calc(var(--x) * 1ch);
                 top:  calc(var(--y) * 1.2em); }
.emacs-line    { white-space: pre; height: 1.2em; }
```

Windows are absolutely positioned within the frame using `ch` (character
width) and `em` (line height) units. This makes the layout scale naturally
with browser zoom.

---

## 6. JSON Wire Protocol

All messages are NDJSON (one JSON object per line, `\n`-terminated).

### 6.1 Emacs → Browser

#### `frame_update` — Incremental redisplay data

```json
{
  "type": "frame_update",
  "faces": {
    "0":  {"fg":"#839496","bg":"#002b36"},
    "4":  {"fg":"#93a1a1","bg":"#002b36","bold":true},
    "12": {"fg":"#2aa198","bg":"#002b36","italic":true,"underline":true}
  },
  "windows": [{
    "id": 3,
    "x": 0, "y": 0, "w": 80, "h": 35,
    "lines": [
      {
        "row": 0,
        "mode_line": false,
        "continued": false,
        "runs": [
          {"face_id": 4,  "text": "#include "},
          {"face_id": 12, "text": "<stdio.h>"}
        ]
      }
    ],
    "cursor": {"row": 5, "col": 2, "type": 0, "active": true}
  }]
}
```

Only dirty lines are included. The browser merges them into its state.

#### `scroll` — Optimized scroll within a window

```json
{"type":"scroll","window_id":3,"delta_rows":-2}
```

The browser shifts line indices by `delta_rows` instead of redrawing.

#### `clear_frame` — Full screen clear

```json
{"type":"clear_frame","bg":"#1e1e1e"}
```

#### `frame_size` — Frame dimensions changed

```json
{"type":"frame_size","cols":80,"rows":50,"default_face":{"fg":"#d4d4d4","bg":"#1e1e1e"}}
```

#### `heartbeat` — Keepalive (sent every ~16ms)

```json
{"type":"heartbeat","ts":1714500000000}
```

Browser uses this to detect when Emacs is busy (no heartbeat for >500ms →
show "busy" overlay).

### 6.2 Browser → Emacs

#### `key` — Keyboard input

```json
{"type":"key","keycode":65,"mods":4,"char":97}
```

- `keycode`: X11 keysym for special keys, or Unicode codepoint
- `mods`: bitfield (shift=1, ctrl=4, meta=8, super=16)
- `char`: Unicode codepoint of the character (0 for special keys)

#### `mouse_down` / `mouse_up` — Mouse button

```json
{"type":"mouse_down","x":120,"y":45,"button":0,"mods":0}
```

- `x`, `y`: pixel coordinates relative to frame container
- `button`: 0=left, 1=middle, 2=right

#### `mouse_move` — Mouse motion (throttled to 60fps)

```json
{"type":"mouse_move","x":130,"y":45,"mods":0}
```

#### `scroll` — Mouse wheel

```json
{"type":"scroll","x":100,"y":200,"dx":0,"dy":3,"mods":0}
```

#### `resize` — Browser window resized

```json
{"type":"resize","cols":120,"rows":40}
```

Character grid dimensions, not pixels. Browser computes from font metrics.

#### `focus` — Window focus change

```json
{"type":"focus","gained":true}
```

#### `font_metrics` — Browser font measurements

```json
{"type":"font_metrics","char_w":8,"char_h":16,"asc":12,"desc":4}
```

Sent on connect. Emacs uses these to map pixel coordinates in mouse events
to character cells.

#### `clipboard` — System clipboard (bidirectional)

```json
{"type":"clipboard","dir":"paste","text":"hello world"}   // browser → Emacs
{"type":"clipboard","dir":"copy","text":"hello world"}    // Emacs → browser
```

Bidirectional clipboard integration (see §3.7):

- **Copy (`dir":"copy"`, Emacs → browser):** M-w / C-w →
  `interprogram-cut-function` (`gui-select-text`) →
  `gui-backend-set-selection` (web method in web-win.el, CLIPBOARD only)
  → `web-set-clipboard` (webterm.c) emits this message. The client
  writes the OS clipboard via the **Electron main process**
  (`electronAPI.writeClipboard` → `ipcMain 'clipboard-write'` →
  `clipboard.writeText`). The renderer's `navigator.clipboard.writeText`
  is rejected ("Document is not focused") because the copy arrives
  outside a user gesture, so the Electron bridge is required.

- **Paste (`dir":"paste"`, browser → Emacs):** sets `Vweb_clipboard`
  (`web-clipboard-text`). Sent both on an in-window paste event and,
  crucially, **whenever the window regains focus** — the client reads
  the OS clipboard (`electronAPI.readClipboard` → `ipcMain
  'clipboard-read'`) and pushes it, so C-y after copying in another app
  sees that copy. `gui-backend-get-selection` for CLIPBOARD returns
  `web-clipboard-text`, falling back to Emacs's own last selection only
  if nothing has synced yet.

#### `interrupt` — Ctrl+C / interrupt button

```json
{"type":"interrupt"}
```

Proxy intercepts this and sends `SIGINT` to its parent (Emacs) rather than
forwarding as JSON.

#### `request_redraw` — (proxy → Emacs only)

```json
{"type":"request_redraw"}
```

Sent by the proxy when a new browser client completes the WebSocket
handshake. Emacs responds with `SET_FRAME_GARBAGED` to trigger a full
redisplay.

---

## 7. Data Flow Walkthrough

### 7.1 Typing a character

```
1. User presses 'a' in browser
2. input.js keydown handler:
   - e.key = "a", charCode = 97
   - sends: {"type":"key","keycode":97,"mods":0,"char":97}
3. WebSocket text frame → proxy → write() to Emacs pipe
4. Emacs event loop: web_proxy_fd_callback() → web_read_socket()
   - Parses JSON line, extracts type="key", keycode=97
   - Creates KEY_EVENT, pushes to keyboard buffer
5. Emacs command loop processes the key
   - self_insert_command inserts 'a' into the buffer
   - Marks window as needing redisplay
6. 60Hz timer fires → Fredisplay(Qt) → redisplay cycle:
   a. web_update_window_begin(w) → allocate json_window
   b. web_draw_glyph_string(s) → for each glyph string:
      - Extract UTF-8 from glyphs, determine face_id
      - Append json_run to current json_line
   c. web_draw_window_cursor(w, ...) → record cursor position
   d. web_update_window_end(w) → clear current_window
   e. web_flush_display(f):
      - Serialize faces dict (only referenced face IDs)
      - Serialize windows array (only dirty lines)
      - Write as NDJSON line to proxy_fd
      - Reset json_state
7. Proxy reads NDJSON line from Emacs pipe
   - Broadcasts as WebSocket text frame to all clients
8. Browser receives message:
   - JSON.parse → state.dispatch(msg)
   - _applyFrameUpdate: merge faces, merge dirty lines
   - _notify() → Preact re-render
   - renderLine() outputs <span> elements with styled text
```

### 7.2 Window split (C-x 2)

```
1. C-x 2 triggers split-window-below
2. Emacs creates new window, adjusts geometries
3. Redisplay cycle runs for both windows
4. web_update_window_begin called for each window
5. web_draw_glyph_string called for all visible lines in both
6. web_flush_display serializes two windows in the windows array
7. Browser receives frame_update with two windows
8. FrameState creates/updates two WindowState entries
9. renderFrame sorts windows by position
10. renderWindow creates two absolutely-positioned divs
11. Each window's lines render independently
```

### 7.3 Browser reconnection

```
1. Browser loses WebSocket connection (network issue, refresh)
2. app.js onclose: schedules reconnect with backoff
3. New WebSocket connection established → proxy accepts
4. Proxy: ws_do_handshake() completes → WS_STATE_OPEN
5. Proxy: request_redraw() → sends {"type":"request_redraw"} to Emacs
6. Emacs: web_read_socket() handles request_redraw
   → SET_FRAME_GARBAGED(f) → marks entire frame as needing redisplay
7. Next redisplay cycle sends complete frame_update with all windows/lines
8. Browser receives full state → FrameState rebuilds everything
9. App renders complete frame
```

---

## 8. Startup Sequence

```
$ ./src/emacs -Q --web

1. Emacs main() processes --web argument
   → Sets initial_window_system to "web"

2. Loads lisp/term/web-win.el
   → Registers "web" in display-format-alist
   → Defines window-system-initialization method

3. window-system-initialization runs
   → Calls x-open-connection("web", ...)
   → C: web_term_init()

4. web_term_init():
   a. Allocates web_display_info (proxy_fd=-1, default metrics)
   b. Allocates I/O buffers (64KB each)
   c. Creates terminal with web_create_terminal()
      - Fills web_redisplay_interface with all RIF function pointers
   d. socketpair(AF_UNIX, SOCK_STREAM) → sv[0], sv[1]
   e. fork()
      - Child: exec("emacs-web-display", "--emacs-fd", sv[1], "--port", "8080")
      - Parent: proxy_fd = sv[0], set O_NONBLOCK
   f. add_read_fd(proxy_fd, web_proxy_fd_callback)

5. Proxy child process starts:
   a. ws_server_create(&server, 8080) → bind + listen on TCP:8080
   b. event_loop() → kqueue/epoll loop begins

6. Emacs creates initial frame
   → x-create-frame-with-faces → web_new_frame()
   → web_set_window_size() → sends frame_size JSON

7. after-init-hook fires
   → web--start-redisplay-timer()
   → Starts 60Hz atimer → web_redisplay_timer_callback every 16ms

8. User opens http://localhost:8000 (static HTTP server for web-client/)
   → Browser loads index.html → app.js
   → WebSocket connects to ws://localhost:8080
   → Proxy accepts, handshake completes
   → Proxy sends request_redraw to Emacs
   → Emacs does full redisplay → browser renders frame
```

---

## 9. Building and Running

### Build Emacs

```bash
cd /path/to/emacs
./autogen.sh       # if building from git
./configure        # generates Makefiles
make -j$(nproc)    # builds src/emacs
```

### Build the proxy

```bash
cd web-display/
make               # produces emacs-web-display binary
```

The Makefile auto-detects the platform and defines `USE_KQUEUE` (macOS) or
`USE_EPOLL` (Linux).

### Run

```bash
# Terminal 1: Start Emacs with web backend
./src/emacs -Q --web

# Terminal 2: Serve the browser client (any static HTTP server works)
cd web-client/
python3 -m http.server 8000

# Browser: open http://localhost:8000
```

Emacs automatically forks the proxy. The proxy listens on port 8080 for
WebSocket connections. The browser client connects to `ws://localhost:8080`
and renders the Emacs frame.

---

## 10. Debug REPL

A built-in debug interface lets you evaluate JavaScript in the browser
from the command line, useful for inspecting state, diagnosing rendering
issues, and testing interactively.

### 10.1 Architecture

```
┌──────────┐  TCP:8081   ┌──────────────┐  WebSocket  ┌──────────┐
│  nc /    │ ──eval───>  │    Proxy     │ ──eval───>  │  Browser │
│  telnet  │ <─result──  │  (port+1)    │ <─result──  │  app.js  │
└──────────┘             └──────────────┘             └──────────┘
```

The proxy opens a secondary TCP listener on **port + 1** (e.g., 8081 when
the WebSocket runs on 8080). Any TCP client can connect and send
JavaScript expressions as plain text lines.

### 10.2 Flow

1. You connect to the debug port: `nc localhost 8081`
2. Type a JavaScript expression, e.g.: `JSON.stringify([..._state.windows.keys()])`
3. Proxy wraps it as `{"type":"eval","code":"..."}` and broadcasts via WebSocket
4. Browser `app.js` receives the `eval` message:
   - Evaluates the code with `(0, eval)(msg.code)` (indirect eval, global scope)
   - Sends result back: `{"type":"eval_result","result":"..."}` or `{"type":"eval_result","error":"..."}`
5. Proxy intercepts `eval_result` messages (doesn't forward to Emacs) and
   writes the raw JSON to the debug TCP client
6. Result appears in your terminal

### 10.3 Usage

```bash
# One-shot query (send expression, wait for result):
(echo 'JSON.stringify([..._state.windows.keys()])'; sleep 2) | nc localhost 8081

# Interactive session:
nc localhost 8081
# Then type expressions line by line
```

### 10.4 Exposed Globals

The browser exposes several objects on `window` for debug access:

| Global | Type | Description |
|--------|------|-------------|
| `_state` | `FrameState` | The central state store (faces, windows, lines) |
| `_ws` | `WebSocket` | The active WebSocket connection |
| `_input` | `InputHandler` | The input event handler instance |

### 10.5 Example Queries

```javascript
// List window IDs and their dimensions
JSON.stringify([..._state.windows.entries()].map(([id,w]) => ({id, w:w.w, h:w.h})))

// Inspect a specific line's text runs
JSON.stringify(_state.windows.get(3)?.lines.get(0))

// Check all registered faces
JSON.stringify(Object.fromEntries(_state.faces))

// Get frame dimensions
JSON.stringify({cols: _state.cols, rows: _state.rows})

// Send a synthetic key event
_ws.send(JSON.stringify({type:'key',keycode:97,mods:0,char:97}) + '\n')

// Check connection state
_ws.readyState
```

### 10.6 Implementation Details

**Proxy (`web-display/main.c`):**
- `create_listener()` — creates the debug TCP socket on port+1
- `process_debug_data()` — reads lines from the debug client, wraps in eval JSON, broadcasts
- `on_client_message()` — intercepts `eval_result` messages and routes to debug client instead of Emacs
- Both kqueue and epoll event loops register the debug listen fd and client fd

**Browser (`web-client/app.js`):**
- `ws.onmessage` handler checks for `msg.type === 'eval'`
- Uses indirect eval `(0, eval)(msg.code)` for global scope access
- Catches eval errors and sends them back as `{"type":"eval_result","error":"..."}`

---

## 11. File Index

### Emacs C source (`src/`)

| File | Lines | Description |
|------|-------|-------------|
| `webterm.h` | ~330 | `web_display_info`, `web_output`, `json_frame_state` structs. Macros: `FRAME_DISPLAY_INFO`, `FRAME_X_OUTPUT`, `FRAME_CURSOR_COLOR`. |
| `webterm.c` | ~2140 | RIF implementation. JSON serialization (`web_flush_display`). Input parsing (`web_read_socket`). Proxy lifecycle (`web_term_init`, `fork`+`exec`). 60Hz timer. Clipboard support. `WR_LIT` macro. |
| `webgui.c` | ~860 | Frame creation (`web_new_frame`), window size (`web_set_window_size`), color allocation (`web_alloc_color`, `web_defined_color`), font handling, cursor color, X11-compatible API surface. |
| `webgui.h` | ~100 | Declarations for webgui.c exports. |

### Elisp (`lisp/term/`)

| File | Lines | Description |
|------|-------|-------------|
| `web-win.el` | 69 | Window system registration, initialization method, 60Hz timer hook. |

### WebSocket Proxy (`web-display/`)

| File | Lines | Description |
|------|-------|-------------|
| `main.c` | ~400 | Event loop (kqueue/epoll), NDJSON line buffering, message forwarding, interrupt handling, client redraw request. |
| `websocket.c` | ~500 | RFC 6455: SHA-1, Base64, HTTP upgrade, frame parsing/encoding, masking, multi-client management. |
| `websocket.h` | ~97 | `ws_server`, `ws_client` structs, `ws_client_state` enum, public API. |
| `Makefile` | 31 | Platform detection, build rules. |

### Browser Client (`web-client/`)

| File | Lines | Description |
|------|-------|-------------|
| `index.html` | 22 | Preact CDN import map, `<div id="app">`, loads `app.js`. |
| `app.js` | ~200 | Root component: WebSocket connect/reconnect, font measurement, resize observer, status bar, busy detection. |
| `state.js` | ~153 | `FrameState`: faces/windows/lines Maps, dispatch, incremental merge, scroll shift, change notification. |
| `input.js` | ~179 | `InputHandler`: keyboard (special keys + Ctrl+letter), mouse (throttled), scroll (clamped), focus, paste. |
| `emacs.css` | ~118 | Monospace grid, cursor blink animations, window positioning, status bar, mode line styling. |
| `components/frame.js` | ~26 | Sorts and renders all windows. |
| `components/window.js` | ~22 | Renders rows 0..h, absolute positioning with `ch`/`em` units. |
| `components/line.js` | ~90 | Renders runs as styled `<span>`s, splits at cursor column, handles cursor types. |

### Legacy / Unused (kept for reference)

| File | Description |
|------|-------------|
| `web-display/protocol.c` | Binary protocol encoder/decoder (superseded by NDJSON). |
| `web-display/protocol.h` | Binary protocol constants and structs. |
| `web-display/framebuffer.c` | Framebuffer for client sync (superseded by incremental JSON). |
| `web-display/framebuffer.h` | Framebuffer struct definitions. |

---

## 12. Future Work

- **Image support**: `IMAGE_GLYPH` currently renders as placeholder spaces.
  Could send image data as base64 or serve via HTTP.
- **True color faces**: The face dictionary could include full RGBA.
- **Selection / copy**: Native text selection works in the browser but
  doesn't feed back to Emacs's kill ring.
- **Multiple frames**: Currently single-frame. Could support multiple
  browser tabs as separate frames.
- **Compression**: Large frame updates could benefit from WebSocket
  per-message compression (permessage-deflate).
- **Authentication**: The WebSocket has no auth. Could add token-based
  access control.
- **HTTPS/WSS**: Currently plaintext. Could add TLS for remote access.
- **Fringe/margin rendering**: Currently no-op. Could render as CSS
  pseudo-elements or narrow columns.
