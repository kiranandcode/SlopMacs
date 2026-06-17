/* Async I/O event loop for the Emacs web display backend.

Copyright (C) 2026 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#ifndef WEB_EVENT_LOOP_H
#define WEB_EVENT_LOOP_H

#ifdef HAVE_PTHREAD

#include <pthread.h>
#include <stdbool.h>
#include <time.h>

enum web_event_type
{
  WEB_EVT_KEY,
  WEB_EVT_MOUSE_DOWN,
  WEB_EVT_MOUSE_UP,
  WEB_EVT_MOUSE_MOVE,
  WEB_EVT_SCROLL,
  WEB_EVT_RESIZE,
  WEB_EVT_FOCUS,
  WEB_EVT_FONT_METRICS,
  WEB_EVT_INTERRUPT,
  WEB_EVT_CLIPBOARD,
  WEB_EVT_REQUEST_REDRAW,
  WEB_EVT_MENU_SELECT,
  WEB_EVT_MENU_CANCEL,
  WEB_EVT_TLDRAW
};

struct web_event
{
  int type;
  int keycode, mods, character;
  int x, y, button;
  int cols, rows;
  int dx, dy;
  int menu_idx;
  bool gained;
  int char_w, char_h, asc, desc;
  char clipboard_dir[16];
  char clipboard_text[4096];
  char *payload;        /* Heap copy of a tldraw_* JSON line (variable
                           length; snapshots exceed any fixed buffer).
                           Freed in web_event_recycle.  */
  struct web_event *next;
};

struct web_event_queue
{
  struct web_event *head, *tail;
  struct web_event *free_list;
  pthread_mutex_t mutex;
  pthread_cond_t nonempty;
  int count;
};

struct web_frame_buffer
{
  unsigned char *data;
  int len, capacity;
};

struct web_frame_output
{
  pthread_mutex_t mutex;
  pthread_cond_t ready_cond;
  struct web_frame_buffer buffers[2];
  int write_idx;
  int ready_idx;
  bool frame_ready;
};

struct web_async_state
{
  pthread_t io_thread;
  volatile bool io_running;
  bool io_started;

  int proxy_fd;
  int notify_pipe[2];

  struct web_event_queue input_queue;
  struct web_frame_output frame_output;

  unsigned char *io_read_buf;
  int io_read_len, io_read_capacity;

  pthread_mutex_t control_mutex;
  unsigned char *control_buf;
  int control_len, control_capacity;

  unsigned char *pending_write_buf;
  int pending_write_len, pending_write_pos, pending_write_capacity;

  int frame_wake_pipe[2];

  struct timespec last_heartbeat;
  volatile bool yield_requested;
  struct timespec last_yield_time;
};

int web_async_init (struct web_async_state *, int);
int web_async_start (struct web_async_state *);
void web_async_shutdown (struct web_async_state *);

void web_event_queue_init (struct web_event_queue *);
void web_event_queue_destroy (struct web_event_queue *);
void web_event_queue_push (struct web_event_queue *, struct web_event *);
struct web_event *web_event_queue_pop (struct web_event_queue *);
struct web_event *web_event_queue_drain (struct web_event_queue *);
struct web_event *web_event_alloc (struct web_event_queue *);
void web_event_recycle (struct web_event_queue *, struct web_event *);

void web_frame_output_init (struct web_frame_output *);
void web_frame_output_destroy (struct web_frame_output *);
void web_frame_output_write (struct web_frame_output *,
			     const unsigned char *, int);
bool web_frame_output_read (struct web_frame_output *,
			    const unsigned char **, int *);

void web_async_enqueue_output (struct web_async_state *,
			       const unsigned char *, int);
void web_frame_output_wake (struct web_async_state *);

#endif /* HAVE_PTHREAD */

#endif /* WEB_EVENT_LOOP_H */
