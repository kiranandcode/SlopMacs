# Fundamental Async Event Loop Rewrite for Emacs Web Backend

## Context

Emacs's core is a **single-threaded synchronous loop** in `keyboard.c:command_loop_1()`:

```
while (true) {
    read_key_sequence()         →  blocks in pselect() waiting for input
    calln(Qcommand_execute)     →  blocks running elisp (can take seconds/minutes)
    safe_run_hooks(post_cmd)    →  blocks
    [loop]
}
```

This means ALL I/O, display, heartbeats, and input processing stops whenever elisp is computing. The browser freezes, shows "busy", and C-g is sluggish. This isn't fixable by adding a helper thread — it requires restructuring the entire event loop into an async, multi-threaded architecture.

## New Architecture: Three-Component Async System

```
┌────────────────────────────────────────────────────────────────────┐
│                     I/O EVENT LOOP THREAD                          │
│                     (src/web_event_loop.c)                         │
│                                                                     │
│  Runs independently. Never touches Lisp objects. Never blocks.     │
│                                                                     │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────┐        │
│  │ poll()      │  │ Parse NDJSON │  │ Heartbeat          │        │
│  │ proxy_fd    │→ │ input events │→ │ every 16ms         │        │
│  │ + output_fd │  │ into queue   │  │ to proxy_fd        │        │
│  └─────────────┘  └──────┬───────┘  └────────────────────┘        │
│                          │                                          │
│                  push to input_queue                                │
│                  write byte to notify_pipe → wakes evaluator       │
│                                                                     │
│         ┌──────────────────────────────┐                           │
│         │ Read from frame_output queue │                           │
│         │ Write JSON to proxy_fd       │                           │
│         └──────────────────────────────┘                           │
└────────────────────────────────────────────────────────────────────┘
            ↕ thread-safe queues + notify pipe
┌────────────────────────────────────────────────────────────────────┐
│                    EVALUATOR (Main Thread)                          │
│                    (restructured keyboard.c + webterm.c)            │
│                                                                     │
│  web_command_loop():                                                │
│    while (true) {                                                   │
│      // NON-BLOCKING: drain notify_pipe, process input_queue       │
│      web_process_pending_events()                                   │
│                                                                     │
│      // Read one key/command from processed input                  │
│      cmd = web_read_next_command()                                  │
│                                                                     │
│      // Execute (yields every ~16ms via enhanced maybe_quit)       │
│      execute_command(cmd)                                           │
│                                                                     │
│      // Redisplay → generate JSON → push to frame_output          │
│      web_redisplay_and_publish()                                    │
│                                                                     │
│      // If no input: wait on notify_pipe (with timeout)            │
│      web_wait_for_events(16ms)                                     │
│    }                                                                │
│                                                                     │
│  Yield mechanism (in maybe_quit during computation):                │
│    web_process_pending_events()   // drain input from I/O thread   │
│    web_redisplay_and_publish()    // update display mid-compute    │
└────────────────────────────────────────────────────────────────────┘
```

### Why Three Components, Not Two

The I/O thread is NOT just a "forwarding proxy." It is a **full event loop** with its own responsibilities:
- Parses NDJSON into structured events (not just forwarding bytes)
- Manages heartbeat timing independently
- Handles proxy connection lifecycle (death detection, cleanup)
- Serializes frame output to NDJSON and manages write buffering
- Controls flow between evaluator and browser

The evaluator is NOT the old `command_loop_1`. It is a **new async command loop** that:
- Never blocks on I/O — reads from a queue
- Yields regularly during computation
- Explicitly publishes display frames
- Uses a notify pipe for efficient wakeup (integrates with existing `pselect` infrastructure)

---

## New Files

### `src/web_event_loop.h` — Async event loop data structures

```c
/* Thread-safe input event queue.  */
struct web_event {
  int type;   /* WEB_EVT_KEY, WEB_EVT_MOUSE_DOWN, etc. */
  int keycode, mods, character;
  int x, y, button;
  int cols, rows;
  int dx, dy;
  bool gained;
  int char_w, char_h;
  char clipboard_text[4096];
  struct web_event *next;
};

struct web_event_queue {
  struct web_event *head, *tail;
  struct web_event *free_list;    /* recycled event nodes */
  pthread_mutex_t mutex;
  pthread_cond_t nonempty;
  int count;
};

/* Frame output double-buffer: evaluator writes, I/O thread reads.  */
struct web_frame_buffer {
  unsigned char *data;
  int len, capacity;
};

struct web_frame_output {
  pthread_mutex_t mutex;
  struct web_frame_buffer buffers[2];  /* double-buffered */
  int write_idx;                       /* evaluator writes to this one */
  bool frame_ready;                    /* new frame available for I/O thread */
  pthread_cond_t ready_cond;
};

/* The complete async loop state.  */
struct web_async_state {
  /* I/O thread */
  pthread_t io_thread;
  volatile bool io_running;

  /* The real proxy fd (OWNED by I/O thread, not the evaluator) */
  int proxy_fd;

  /* Notify pipe: I/O thread writes to [1], evaluator's pselect watches [0] */
  int notify_pipe[2];

  /* Input queue: I/O thread pushes, evaluator pops */
  struct web_event_queue input_queue;

  /* Frame output: evaluator pushes, I/O thread reads and sends */
  struct web_frame_output frame_output;

  /* I/O thread read buffer (for parsing NDJSON from proxy) */
  unsigned char *io_read_buf;
  int io_read_len, io_read_capacity;

  /* Heartbeat tracking */
  struct timespec last_heartbeat;

  /* Yield control: set by SIGALRM, checked by evaluator */
  volatile bool yield_requested;
  struct timespec last_yield_time;
};
```

### `src/web_event_loop.c` — I/O event loop thread + queue operations

**Event queue operations:**
- `web_event_queue_init()` / `web_event_queue_destroy()`
- `web_event_queue_push()` — I/O thread pushes parsed events (locks mutex)
- `web_event_queue_pop()` — evaluator pops one event (locks mutex, non-blocking)
- `web_event_queue_drain()` — evaluator drains all events into local list (minimal lock time)

**Frame output operations:**
- `web_frame_output_init()` / `web_frame_output_destroy()`
- `web_frame_output_begin_write()` — evaluator starts writing (returns buffer pointer)
- `web_frame_output_commit()` — evaluator finishes writing, swaps buffer, signals I/O thread
- `web_frame_output_read()` — I/O thread reads committed frame (locks mutex)

**I/O thread main function: `web_io_thread_func()`**

This is the core of the new event loop:

```c
void *web_io_thread_func(void *arg) {
    struct web_async_state *state = arg;

    /* Block all signals — they go to the main (evaluator) thread.  */
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    while (state->io_running) {
        /* ---- PHASE 1: Poll for I/O ---- */
        struct pollfd fds[2];
        fds[0].fd = state->proxy_fd;          fds[0].events = POLLIN;
        fds[1].fd = /* output signal pipe */;  fds[1].events = POLLIN;

        int timeout_ms = 16;  /* heartbeat interval */
        int n = poll(fds, 2, timeout_ms);

        /* ---- PHASE 2: Read proxy input, parse, enqueue ---- */
        if (fds[0].revents & POLLIN) {
            web_io_read_proxy(state);      /* read NDJSON from proxy */
            web_io_parse_events(state);    /* parse into web_event structs */
            web_io_enqueue_events(state);  /* push to input_queue */
            /* Wake evaluator via notify pipe */
            char c = 1;
            write(state->notify_pipe[1], &c, 1);
        }

        /* ---- PHASE 3: Send pending frame data to proxy ---- */
        if (state->frame_output.frame_ready) {
            web_io_send_frame(state);      /* read from frame_output, write to proxy */
        }

        /* ---- PHASE 4: Heartbeat ---- */
        web_io_maybe_send_heartbeat(state);

        /* ---- PHASE 5: Check for proxy death ---- */
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            state->io_running = false;
            break;
        }
    }
    return NULL;
}
```

**Key I/O thread functions:**

- `web_io_read_proxy()` — Non-blocking read from proxy_fd into io_read_buf
- `web_io_parse_events()` — Scan for complete NDJSON lines, parse each into `web_event` structs (using the same JSON parsing logic currently in `web_read_socket`)
- `web_io_enqueue_events()` — Push parsed events to `input_queue`
- `web_io_send_frame()` — Lock frame_output mutex, read committed frame buffer, write to proxy_fd, unlock
- `web_io_maybe_send_heartbeat()` — Check elapsed time, write heartbeat JSON directly to proxy_fd

---

## Modified Files — Fundamental Changes

### `src/webterm.h` — Add async state to `web_display_info`

Add at the end of the struct (after `json_state`):

```c
#ifdef HAVE_PTHREAD
  /* Async event loop state.  */
  struct web_async_state async;
  bool async_enabled;
#endif
```

### `src/webterm.c` — Complete restructuring of I/O path

**a) `web_term_init()` — Start I/O thread instead of registering proxy_fd directly**

After fork+exec of proxy, instead of:
```c
add_read_fd(dpyinfo->proxy_fd, web_proxy_fd_callback, dpyinfo);
```

Do:
```c
#ifdef HAVE_PTHREAD
  /* Initialize async event loop.  */
  web_async_init(&dpyinfo->async, dpyinfo->proxy_fd);

  /* The evaluator uses the notify pipe for wakeup, not the proxy fd.
     Register notify_pipe[0] with Emacs's event loop so pselect() wakes
     up when the I/O thread has input ready.  */
  dpyinfo->proxy_fd = -1;  /* Evaluator no longer owns real proxy fd */
  add_read_fd(dpyinfo->async.notify_pipe[0], web_notify_callback, dpyinfo);

  /* Start I/O event loop thread.  */
  web_async_start(&dpyinfo->async);
  dpyinfo->async_enabled = true;
#else
  add_read_fd(dpyinfo->proxy_fd, web_proxy_fd_callback, dpyinfo);
#endif
```

**b) `web_read_socket()` — Read from event queue, not proxy fd**

Completely rewritten when async is enabled. Instead of non-blocking read from proxy_fd + NDJSON parsing, it:
1. Drains the notify pipe (consume wakeup bytes)
2. Pops events from `input_queue`
3. Converts `web_event` structs to Emacs `struct input_event`
4. Stores via `kbd_buffer_store_event()`

The JSON parsing logic moves to the I/O thread (`web_io_parse_events`).

```c
static int
web_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  struct web_display_info *dpyinfo = terminal->display_info.web;
  int count = 0;

#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      /* Drain notify pipe.  */
      char drain[64];
      while (read(dpyinfo->async.notify_pipe[0], drain, sizeof drain) > 0)
        ;

      /* Pop events from input queue.  */
      struct web_event *events = web_event_queue_drain(&dpyinfo->async.input_queue);

      for (struct web_event *e = events; e; )
        {
          struct web_event *next = e->next;
          count += web_dispatch_event(dpyinfo, e, hold_quit);
          web_event_recycle(&dpyinfo->async.input_queue, e);
          e = next;
        }
      return count;
    }
#endif

  /* Fallback: original synchronous read from proxy_fd.  */
  /* ... existing web_read_socket code ... */
}
```

**c) `web_dispatch_event()` — New function, converts web_event to Emacs input_event**

Extracted from the current `web_read_socket` JSON parsing code. Takes a `struct web_event` and creates appropriate Emacs events (KEY_EVENT, MOUSE_CLICK_EVENT, WHEEL_EVENT, etc.).

**d) `web_flush_display()` — Write to frame output buffer, not proxy fd**

When async is enabled, instead of calling `web_write_flush()` (which writes to proxy_fd), it commits the write buffer to the frame output double-buffer:

```c
/* At end of web_flush_display, replace web_write_flush: */
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      web_frame_output_write(&dpyinfo->async.frame_output,
                             dpyinfo->write_buf, dpyinfo->write_buf_len);
      dpyinfo->write_buf_len = 0;
      return;
    }
#endif
  web_write_flush(dpyinfo);
```

**e) `web_redisplay_timer_callback()` — No heartbeat, just trigger redisplay**

```c
static void
web_redisplay_timer_callback (struct atimer *timer)
{
  struct web_display_info *dpyinfo = web_display_info;
  if (!web_frame_ready || !dpyinfo) return;

#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      /* I/O thread handles heartbeats. Timer only triggers redisplay
         (which generates a frame → I/O thread sends it).  */
      if (!redisplaying_p && NILP (Vinhibit_redisplay)
          && !interrupt_input_blocked)
        Fredisplay (Qt);
      return;
    }
#endif

  /* Fallback: original heartbeat + redisplay.  */
  /* ... existing code ... */
}
```

**f) `web_delete_terminal()` — Shut down I/O thread**

```c
static void
web_delete_terminal (struct terminal *terminal)
{
  struct web_display_info *dpyinfo = terminal->display_info.web;
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    web_async_shutdown(&dpyinfo->async);
#endif
  /* ... existing cleanup ... */
}
```

### `src/keyboard.c` — New web async command loop

**New function: `web_command_loop()`** — replaces `command_loop_1()` for web backend.

This is the fundamental restructuring. Instead of the synchronous `read_key_sequence → execute → loop`, it is an **event-driven loop**:

```c
#ifdef HAVE_WEB
static Lisp_Object
web_command_loop_1 (void)
{
  /* Same initialization as command_loop_1 lines 1320-1357 */
  kset_prefix_arg (current_kboard, Qnil);
  /* ... */

  while (true)
    {
      if (!FRAME_LIVE_P (XFRAME (selected_frame)))
        Fkill_emacs (Qnil, Qnil);
      set_buffer_internal (XBUFFER (XWINDOW (selected_window)->contents));

      /* ---- PHASE 1: Process ALL pending events (non-blocking) ---- */
      web_process_pending_events ();

      /* ---- PHASE 2: Redisplay if needed ---- */
      if (web_needs_redisplay_p ())
        web_redisplay_and_publish ();

      /* ---- PHASE 3: Read key sequence (non-blocking first, then blocking) ---- */
      Vdeactivate_mark = Qnil;
      Vthis_command = Qnil;
      Vreal_this_command = Qnil;

      raw_keybuf_count = 0;
      Lisp_Object keybuf[READ_KEY_ELTS];

      /* read_key_sequence uses wait_reading_process_output internally,
         which will block on the notify_pipe[0] fd.  When the I/O thread
         pushes input, it writes to notify_pipe[1], waking pselect().
         The fd callback (web_notify_callback) fires, which calls
         web_read_socket, which drains the event queue.

         This means read_key_sequence works unchanged — it just waits
         on a different fd than before. */
      int i = read_key_sequence (keybuf, Qnil, false, true, true, false, false);

      if (i == 0) return Qnil;
      if (i == -1) { cancel_echoing (); this_command_key_count = 0; goto finalize; }

      /* ---- PHASE 4: Execute command ---- */
      last_command_event = keybuf[i - 1];
      Lisp_Object cmd = read_key_sequence_cmd;
      Vthis_command = cmd;
      Vreal_this_command = cmd;

      safe_run_hooks_maybe_narrowed (Qpre_command_hook,
                                     XWINDOW (selected_window));

      if (NILP (Vthis_command))
        call0 (Qundefined);
      else
        {
          call0 (Qundo_auto__add_boundary);
          calln (Qcommand_execute, Vthis_command);
        }

      /* ---- PHASE 5: Post-command processing ---- */
      safe_run_hooks_maybe_narrowed (Qpost_command_hook,
                                     XWINDOW (selected_window));
      kset_last_command (current_kboard, Vthis_command);
      kset_real_last_command (current_kboard, Vreal_this_command);

    finalize:
      this_command_key_count = 0;

      /* ---- PHASE 6: Publish display frame ---- */
      web_redisplay_and_publish ();
    }
}
#endif /* HAVE_WEB */
```

**Hook into `command_loop()`** (line 1113): When web backend is active, call `web_command_loop_1` instead of `command_loop_1`:

```c
Lisp_Object
command_loop (void)
{
#ifdef HAVE_WEB
  if (web_async_active_p ())
    {
      /* Use the async web command loop.  */
      if (command_loop_level > 0 || minibuf_level > 0)
        return internal_catch (Qexit, web_command_loop_2, Qerror);
      else
        while (1)
          {
            internal_catch (Qtop_level, top_level_1, Qnil);
            internal_catch (Qtop_level, web_command_loop_2, Qerror);
            executing_kbd_macro = Qnil;
            if (noninteractive) Fkill_emacs (Qt, Qnil);
          }
    }
#endif
  /* ... original command_loop code ... */
}
```

**`web_process_pending_events()`** — Drains the input queue and processes all pending events:
```c
static void
web_process_pending_events (void)
{
  /* This triggers the fd callback on notify_pipe[0], which calls
     web_read_socket, which drains the event queue.  */
  if (detect_input_pending_run_timers (true))
    swallow_events (true);
}
```

**`web_redisplay_and_publish()`** — Redisplay + push frame to I/O thread:
```c
static void
web_redisplay_and_publish (void)
{
  if (!redisplaying_p && NILP (Vinhibit_redisplay))
    {
      redisplay_preserve_echo_area (2);
      /* Frame data was generated by web_flush_display and committed
         to the frame_output buffer. The I/O thread will pick it up
         and send to the proxy.  */
    }
}
```

### `src/bytecode.c` — Yield on every backward branch + trigger events/redisplay

```c
CASE (Bgoto):
  arg = FETCH2;
op_branch:
  {
    const unsigned char *new_pc = bytestr_data + arg;
    if (new_pc < pc)
      {
        /* Backward branch. Process pending signals immediately so the
           async I/O thread's input gets processed and the display
           updates during long computations.  */
        maybe_quit ();

        /* Time-based redisplay: if >16ms since last yield, do a
           full event processing + redisplay cycle.  */
#ifdef HAVE_WEB
        if (web_async_active_p () && web_yield_due_p ())
          {
            web_process_pending_events ();
            web_redisplay_and_publish ();
          }
#endif

        quitcounter++;
        if (!quitcounter)
          {
            quitcounter = 1;
            maybe_gc ();
          }
      }
    pc = new_pc;
    NEXT;
  }
```

**`web_yield_due_p()`** — Time-based check (~16ms interval):
```c
bool web_yield_due_p (void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = /* compute ms since last yield */;
    if (elapsed_ms >= 16) {
        web_last_yield_time = now;
        return true;
    }
    return false;
}
```

### `src/eval.c` — Yield during function calls

```c
Lisp_Object
eval_sub (Lisp_Object form)
{
  maybe_quit ();
#ifdef HAVE_WEB
  /* During web async mode, yield at function evaluation boundaries
     to keep the display responsive during deep evaluation.  */
  if (web_async_active_p () && web_yield_due_p ())
    {
      web_process_pending_events ();
      web_redisplay_and_publish ();
    }
#endif
  /* ... rest of eval_sub ... */
}
```

### `configure.ac` — Build integration

Add `web_event_loop.o` to `WEB_OBJ`:
```
WEB_OBJ="webterm.o webfns.o webfont.o web_event_loop.o"
```

### `src/Makefile.in` — Build rule

Add `web_event_loop.o` to the web object list.

---

## Thread Communication Protocol

### Input Flow (Browser → Evaluator)

```
Browser sends JSON via WebSocket
    ↓
Proxy forwards NDJSON to Emacs via socketpair
    ↓
I/O thread: poll() wakes on proxy_fd
I/O thread: read() into io_read_buf
I/O thread: scan for \n-delimited lines
I/O thread: parse JSON → create web_event struct
I/O thread: pthread_mutex_lock(input_queue.mutex)
I/O thread: append to input_queue linked list
I/O thread: pthread_mutex_unlock(input_queue.mutex)
I/O thread: write(notify_pipe[1], "\x01", 1)  ← wake evaluator
    ↓
Evaluator: pselect() wakes on notify_pipe[0]
           OR maybe_quit() → process_pending_signals() → gobble_input()
           → web_read_socket() (via read_socket_hook)
Evaluator: read(notify_pipe[0]) to drain
Evaluator: web_event_queue_drain() → get all events
Evaluator: for each event → web_dispatch_event() → kbd_buffer_store_event()
```

### Output Flow (Evaluator → Browser)

```
Evaluator: Fredisplay(Qt) triggers redisplay cycle
Evaluator: RIF functions accumulate JSON in write_buf (unchanged)
Evaluator: web_flush_display() at end of cycle:
           web_frame_output_write(frame_output, write_buf, len)
           → copies write_buf into frame_output double-buffer
           → sets frame_ready = true
           → pthread_cond_signal(ready_cond)
    ↓
I/O thread: poll() or cond_wait detects frame_ready
I/O thread: pthread_mutex_lock(frame_output.mutex)
I/O thread: read committed buffer
I/O thread: frame_ready = false
I/O thread: pthread_mutex_unlock(frame_output.mutex)
I/O thread: write() to proxy_fd
    ↓
Proxy broadcasts to all WebSocket clients
    ↓
Browser renders
```

### Heartbeat Flow (Independent)

```
I/O thread: every 16ms (poll timeout):
I/O thread:   clock_gettime() → build heartbeat JSON in stack buffer
I/O thread:   write(proxy_fd, heartbeat, len)
              No mutex needed — I/O thread is sole writer to proxy_fd
```

### Interrupt Flow (C-g)

```
Browser sends {"type":"interrupt"}
    ↓
Proxy intercepts → kill(getppid(), SIGINT) [existing behavior]
    ↓
SIGINT delivered to main (evaluator) thread
    ↓
Sets Vquit_flag
    ↓
Next maybe_quit() (every backward branch) → throws to quit handler
```

---

## Files Summary

### New Files
| File | Purpose |
|------|---------|
| `src/web_event_loop.h` | Async state structs, event queue, frame buffer, API |
| `src/web_event_loop.c` | I/O thread main loop, event queue ops, frame output ops, init/shutdown |

### Fundamentally Modified Files
| File | Changes |
|------|---------|
| `src/webterm.h` | Add `struct web_async_state` + `async_enabled` flag to `web_display_info` |
| `src/webterm.c` | `web_term_init` starts I/O thread; `web_read_socket` reads from queue; `web_flush_display` writes to frame buffer; timer callback simplified; `web_delete_terminal` shuts down I/O thread; JSON parsing extracted to I/O thread |
| `src/keyboard.c` | New `web_command_loop_1()` async command loop; hook in `command_loop()` to use it; helper functions `web_process_pending_events()`, `web_redisplay_and_publish()` |
| `src/bytecode.c` | `maybe_quit()` on every backward branch + time-based `web_yield_due_p()` for event processing + redisplay during computation |
| `src/eval.c` | Yield point in `eval_sub()` for deep evaluation chains |
| `configure.ac` | Add `web_event_loop.o` to `WEB_OBJ` |
| `src/Makefile.in` | Build rule for `web_event_loop.o` |

---

## Safety Invariants

1. **I/O thread NEVER accesses Lisp objects** — only raw bytes, parsed ints/strings, and its own buffers
2. **Evaluator NEVER writes to proxy_fd** — all output goes through frame_output queue
3. **Notify pipe is the ONLY fd the evaluator watches** for web input — integrates cleanly with existing `pselect()` in `wait_reading_process_output()`
4. **I/O thread blocks ALL signals** — SIGALRM and SIGINT go to evaluator thread
5. **Double-buffered frame output** — evaluator writes to back buffer, I/O thread reads front buffer, swap is a pointer exchange under mutex
6. **Event queue uses recycled nodes** — malloc'd once, reused via free_list, no allocation in hot path
7. **Graceful fallback** — `#ifdef HAVE_PTHREAD` / `async_enabled` flag allows single-threaded fallback

---

## Verification

```bash
# Build
make -j$(nproc)
cd web-display && make && cd ..

# Run
./src/emacs -Q --web
# Serve web-client: cd web-client && python3 -m http.server 8000
# Open http://localhost:8000
```

### Test Cases

1. **Long computation**: `(dotimes (i 100000000) nil)`
   - Browser NEVER shows "busy" (heartbeats flow from I/O thread)
   - Display updates during computation (yield every ~16ms)
   - C-g interrupts within milliseconds

2. **Display updates during compute**: `(dotimes (i 1000000) (message "step %d" i))`
   - Messages visible in minibuffer at 60fps during loop
   - Not just one final update after loop completes

3. **Input during compute**: Start long computation, type C-g
   - C-g processed immediately (SIGINT from proxy)
   - Other keys buffered and processed when computation yields

4. **Normal editing**: Typing, scrolling, split windows, M-x, etc.
   - Works identically to single-threaded mode

5. **Process I/O**: `M-x shell`, run commands
   - Process output appears while other operations run

6. **Clean shutdown**: `C-x C-c`
   - I/O thread stops cleanly, no zombie threads
