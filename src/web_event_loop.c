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

#include <config.h>

#ifdef HAVE_PTHREAD

#include "web_event_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WEB_IO_READ_INIT_SIZE (64 * 1024)
#define WEB_OUTPUT_INIT_SIZE (64 * 1024)
#define WEB_HEARTBEAT_MS 5000
#define WEB_POLL_TIMEOUT_MS 50

static int
web_set_nonblocking (int fd)
{
  int flags = fcntl (fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

static bool
web_buffer_reserve (unsigned char **data, int *capacity, int need)
{
  if (need <= *capacity)
    return true;

  int new_capacity = *capacity ? *capacity * 2 : WEB_OUTPUT_INIT_SIZE;
  while (new_capacity < need)
    new_capacity *= 2;

  unsigned char *new_data = realloc (*data, new_capacity);
  if (!new_data)
    return false;

  *data = new_data;
  *capacity = new_capacity;
  return true;
}

static void
web_pending_append (struct web_async_state *state,
		    const unsigned char *data, int len)
{
  if (len <= 0)
    return;

  if (state->pending_write_pos == state->pending_write_len)
    state->pending_write_pos = state->pending_write_len = 0;
  else if (state->pending_write_pos > 0)
    {
      int remaining = state->pending_write_len - state->pending_write_pos;
      memmove (state->pending_write_buf,
	       state->pending_write_buf + state->pending_write_pos,
	       remaining);
      state->pending_write_pos = 0;
      state->pending_write_len = remaining;
    }

  int need = state->pending_write_len + len;
  if (!web_buffer_reserve (&state->pending_write_buf,
			   &state->pending_write_capacity, need))
    return;

  memcpy (state->pending_write_buf + state->pending_write_len, data, len);
  state->pending_write_len += len;
}

static void
web_pending_flush (struct web_async_state *state)
{
  while (state->pending_write_pos < state->pending_write_len)
    {
      ssize_t n = write (state->proxy_fd,
			 state->pending_write_buf + state->pending_write_pos,
			 state->pending_write_len - state->pending_write_pos);
      if (n > 0)
	{
	  state->pending_write_pos += n;
	  continue;
	}
      if (n < 0 && errno == EINTR)
	continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	return;

      state->io_running = false;
      return;
    }

  state->pending_write_pos = state->pending_write_len = 0;
}

void
web_event_queue_init (struct web_event_queue *queue)
{
  memset (queue, 0, sizeof *queue);
  pthread_mutex_init (&queue->mutex, NULL);
  pthread_cond_init (&queue->nonempty, NULL);
}

void
web_event_queue_destroy (struct web_event_queue *queue)
{
  struct web_event *event = queue->head;
  while (event)
    {
      struct web_event *next = event->next;
      free (event);
      event = next;
    }

  event = queue->free_list;
  while (event)
    {
      struct web_event *next = event->next;
      free (event);
      event = next;
    }

  pthread_cond_destroy (&queue->nonempty);
  pthread_mutex_destroy (&queue->mutex);
}

struct web_event *
web_event_alloc (struct web_event_queue *queue)
{
  pthread_mutex_lock (&queue->mutex);
  struct web_event *event = queue->free_list;
  if (event)
    queue->free_list = event->next;
  pthread_mutex_unlock (&queue->mutex);

  if (!event)
    event = malloc (sizeof *event);
  if (event)
    memset (event, 0, sizeof *event);
  return event;
}

void
web_event_recycle (struct web_event_queue *queue, struct web_event *event)
{
  if (!event)
    return;

  memset (event, 0, sizeof *event);
  pthread_mutex_lock (&queue->mutex);
  event->next = queue->free_list;
  queue->free_list = event;
  pthread_mutex_unlock (&queue->mutex);
}

void
web_event_queue_push (struct web_event_queue *queue, struct web_event *event)
{
  event->next = NULL;

  pthread_mutex_lock (&queue->mutex);
  if (queue->tail)
    queue->tail->next = event;
  else
    queue->head = event;
  queue->tail = event;
  queue->count++;
  pthread_cond_signal (&queue->nonempty);
  pthread_mutex_unlock (&queue->mutex);
}

struct web_event *
web_event_queue_pop (struct web_event_queue *queue)
{
  pthread_mutex_lock (&queue->mutex);
  struct web_event *event = queue->head;
  if (event)
    {
      queue->head = event->next;
      if (!queue->head)
	queue->tail = NULL;
      queue->count--;
      event->next = NULL;
    }
  pthread_mutex_unlock (&queue->mutex);
  return event;
}

struct web_event *
web_event_queue_drain (struct web_event_queue *queue)
{
  pthread_mutex_lock (&queue->mutex);
  struct web_event *events = queue->head;
  queue->head = queue->tail = NULL;
  queue->count = 0;
  pthread_mutex_unlock (&queue->mutex);
  return events;
}

void
web_frame_output_init (struct web_frame_output *output)
{
  memset (output, 0, sizeof *output);
  pthread_mutex_init (&output->mutex, NULL);
  pthread_cond_init (&output->ready_cond, NULL);
}

void
web_frame_output_destroy (struct web_frame_output *output)
{
  for (int i = 0; i < 2; ++i)
    free (output->buffers[i].data);
  pthread_cond_destroy (&output->ready_cond);
  pthread_mutex_destroy (&output->mutex);
}

void
web_frame_output_write (struct web_frame_output *output,
			const unsigned char *data, int len)
{
  if (len <= 0)
    return;

  pthread_mutex_lock (&output->mutex);

  struct web_frame_buffer *buffer = output->frame_ready
    ? &output->buffers[output->ready_idx]
    : &output->buffers[output->write_idx];
  int old_len = output->frame_ready ? buffer->len : 0;
  if (web_buffer_reserve (&buffer->data, &buffer->capacity, old_len + len))
    {
      memcpy (buffer->data + old_len, data, len);
      buffer->len = old_len + len;
      if (!output->frame_ready)
	{
	  output->ready_idx = output->write_idx;
	  output->write_idx = 1 - output->write_idx;
	}
      output->frame_ready = true;
      pthread_cond_signal (&output->ready_cond);
    }

  pthread_mutex_unlock (&output->mutex);
}

void
web_frame_output_wake (struct web_async_state *state)
{
  unsigned char byte = 1;
  ssize_t ignored = write (state->frame_wake_pipe[1], &byte, 1);
  (void) ignored;
}

bool
web_frame_output_read (struct web_frame_output *output,
		       const unsigned char **data, int *len)
{
  bool ready = false;
  pthread_mutex_lock (&output->mutex);
  if (output->frame_ready)
    {
      struct web_frame_buffer *buffer = &output->buffers[output->ready_idx];
      *data = buffer->data;
      *len = buffer->len;
      output->frame_ready = false;
      ready = true;
    }
  pthread_mutex_unlock (&output->mutex);
  return ready;
}

void
web_async_enqueue_output (struct web_async_state *state,
			  const unsigned char *data, int len)
{
  if (len <= 0)
    return;

  pthread_mutex_lock (&state->control_mutex);
  int need = state->control_len + len;
  if (web_buffer_reserve (&state->control_buf, &state->control_capacity, need))
    {
      memcpy (state->control_buf + state->control_len, data, len);
      state->control_len += len;
    }
  pthread_mutex_unlock (&state->control_mutex);
}

static const char *
json_find_key (const char *json, int json_len, const char *key)
{
  int klen = strlen (key);
  for (int i = 0; i + klen + 3 < json_len; ++i)
    if (json[i] == '"'
	&& memcmp (json + i + 1, key, klen) == 0
	&& json[i + 1 + klen] == '"'
	&& json[i + 2 + klen] == ':')
      return json + i + 3 + klen;
  return NULL;
}

static int
json_extract_int (const char *json, int json_len, const char *key)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return 0;
  while (*v == ' ')
    ++v;
  return atoi (v);
}

static bool
json_extract_bool (const char *json, int json_len, const char *key)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return false;
  while (*v == ' ')
    ++v;
  return *v == 't';
}

static int
json_extract_string (const char *json, int json_len, const char *key,
		     char *buf, int buf_size)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return -1;
  while (*v == ' ')
    ++v;
  if (*v != '"')
    return -1;

  ++v;
  int len = 0;
  while (*v && *v != '"' && len < buf_size - 1)
    {
      if (*v == '\\' && v[1])
	{
	  ++v;
	  switch (*v)
	    {
	    case 'n': buf[len++] = '\n'; break;
	    case 'r': buf[len++] = '\r'; break;
	    case 't': buf[len++] = '\t'; break;
	    case '"': buf[len++] = '"'; break;
	    case '\\': buf[len++] = '\\'; break;
	    default: buf[len++] = *v; break;
	    }
	}
      else
	buf[len++] = *v;
      ++v;
    }

  buf[len] = '\0';
  return len;
}

static bool
web_parse_event (const char *line, int line_len, struct web_event *event)
{
  char type[64];
  if (json_extract_string (line, line_len, "type", type, sizeof type) < 0)
    return false;

  if (strcmp (type, "key") == 0)
    {
      event->type = WEB_EVT_KEY;
      event->keycode = json_extract_int (line, line_len, "keycode");
      event->mods = json_extract_int (line, line_len, "mods");
      event->character = json_extract_int (line, line_len, "char");
      return true;
    }
  if (strcmp (type, "mouse_down") == 0 || strcmp (type, "mouse_up") == 0)
    {
      event->type = strcmp (type, "mouse_down") == 0
	? WEB_EVT_MOUSE_DOWN : WEB_EVT_MOUSE_UP;
      event->x = json_extract_int (line, line_len, "x");
      event->y = json_extract_int (line, line_len, "y");
      event->button = json_extract_int (line, line_len, "button");
      event->mods = json_extract_int (line, line_len, "mods");
      return true;
    }
  if (strcmp (type, "mouse_move") == 0)
    {
      event->type = WEB_EVT_MOUSE_MOVE;
      event->x = json_extract_int (line, line_len, "x");
      event->y = json_extract_int (line, line_len, "y");
      return true;
    }
  if (strcmp (type, "scroll") == 0)
    {
      event->type = WEB_EVT_SCROLL;
      event->x = json_extract_int (line, line_len, "x");
      event->y = json_extract_int (line, line_len, "y");
      event->dx = json_extract_int (line, line_len, "dx");
      event->dy = json_extract_int (line, line_len, "dy");
      event->mods = json_extract_int (line, line_len, "mods");
      return true;
    }
  if (strcmp (type, "resize") == 0)
    {
      event->type = WEB_EVT_RESIZE;
      event->cols = json_extract_int (line, line_len, "cols");
      event->rows = json_extract_int (line, line_len, "rows");
      return true;
    }
  if (strcmp (type, "focus") == 0)
    {
      event->type = WEB_EVT_FOCUS;
      event->gained = json_extract_bool (line, line_len, "gained");
      return true;
    }
  if (strcmp (type, "font_metrics") == 0)
    {
      event->type = WEB_EVT_FONT_METRICS;
      event->char_w = json_extract_int (line, line_len, "char_w");
      event->char_h = json_extract_int (line, line_len, "char_h");
      event->asc = json_extract_int (line, line_len, "asc");
      event->desc = json_extract_int (line, line_len, "desc");
      return true;
    }
  if (strcmp (type, "interrupt") == 0)
    {
      /* C-g must work while the evaluator is busy, but the event
	 queue is only drained when it reads input -- so act here, on
	 the I/O thread, by raising SIGINT (delivered to the main
	 thread, whose handler just sets Vquit_flag; the evaluator
	 quits at its next quit check).  This path is used when the
	 proxy runs in --emacs-port mode and forwards interrupts as
	 data; in legacy --emacs-fd mode the proxy SIGINTs us directly
	 and this line is never parsed.  Still enqueue the event as a
	 fallback for the synchronous (non-threaded) build.  */
      kill (getpid (), SIGINT);
      event->type = WEB_EVT_INTERRUPT;
      return true;
    }
  if (strcmp (type, "clipboard") == 0)
    {
      event->type = WEB_EVT_CLIPBOARD;
      json_extract_string (line, line_len, "dir",
			   event->clipboard_dir, sizeof event->clipboard_dir);
      json_extract_string (line, line_len, "text",
			   event->clipboard_text, sizeof event->clipboard_text);
      return true;
    }
  if (strcmp (type, "request_redraw") == 0)
    {
      event->type = WEB_EVT_REQUEST_REDRAW;
      return true;
    }
  if (strcmp (type, "menu_select") == 0)
    {
      event->type = WEB_EVT_MENU_SELECT;
      event->menu_idx = json_extract_int (line, line_len, "idx");
      return true;
    }
  if (strcmp (type, "menu_cancel") == 0)
    {
      event->type = WEB_EVT_MENU_CANCEL;
      return true;
    }

  return false;
}

static void
web_io_notify_evaluator (struct web_async_state *state)
{
  unsigned char byte = 1;
  ssize_t ignored = write (state->notify_pipe[1], &byte, 1);
  (void) ignored;
}

static void
web_io_read_proxy (struct web_async_state *state)
{
  while (state->io_running)
    {
      if (state->io_read_len == state->io_read_capacity
	  && !web_buffer_reserve (&state->io_read_buf,
				  &state->io_read_capacity,
				  state->io_read_capacity + WEB_IO_READ_INIT_SIZE))
	return;

      ssize_t n = read (state->proxy_fd,
			state->io_read_buf + state->io_read_len,
			state->io_read_capacity - state->io_read_len);
      if (n > 0)
	{
	  state->io_read_len += n;
	  continue;
	}
      if (n == 0)
	{
	  state->io_running = false;
	  return;
	}
      if (errno == EINTR)
	continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
	return;

      state->io_running = false;
      return;
    }
}

static void
web_io_parse_events (struct web_async_state *state)
{
  int pos = 0;
  bool pushed = false;

  while (pos < state->io_read_len)
    {
      int nl = -1;
      for (int i = pos; i < state->io_read_len; ++i)
	if (state->io_read_buf[i] == '\n')
	  {
	    nl = i;
	    break;
	  }

      if (nl < 0)
	break;

      state->io_read_buf[nl] = '\0';
      const char *line = (const char *) state->io_read_buf + pos;
      int line_len = nl - pos;
      pos = nl + 1;

      if (line_len < 2)
	continue;

      struct web_event *event = web_event_alloc (&state->input_queue);
      if (!event)
	continue;

      if (web_parse_event (line, line_len, event))
	{
	  web_event_queue_push (&state->input_queue, event);
	  pushed = true;
	}
      else
	web_event_recycle (&state->input_queue, event);
    }

  if (pos > 0)
    {
      state->io_read_len -= pos;
      if (state->io_read_len > 0)
	memmove (state->io_read_buf, state->io_read_buf + pos,
		 state->io_read_len);
    }

  if (pushed)
    web_io_notify_evaluator (state);
}

static long
web_elapsed_ms (struct timespec newer, struct timespec older)
{
  return (newer.tv_sec - older.tv_sec) * 1000
    + (newer.tv_nsec - older.tv_nsec) / 1000000;
}

static void
web_io_maybe_send_heartbeat (struct web_async_state *state)
{
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC, &now);
  if (web_elapsed_ms (now, state->last_heartbeat) < WEB_HEARTBEAT_MS)
    return;

  state->last_heartbeat = now;
  unsigned long long ms = (unsigned long long) now.tv_sec * 1000
    + (unsigned long long) now.tv_nsec / 1000000;
  char heartbeat[128];
  int len = snprintf (heartbeat, sizeof heartbeat,
		      "{\"type\":\"heartbeat\",\"ts\":%llu}\n", ms);
  if (len > 0)
    web_pending_append (state, (const unsigned char *) heartbeat, len);
}

static void
web_io_drain_control_output (struct web_async_state *state)
{
  pthread_mutex_lock (&state->control_mutex);
  if (state->control_len > 0)
    {
      web_pending_append (state, state->control_buf, state->control_len);
      state->control_len = 0;
    }
  pthread_mutex_unlock (&state->control_mutex);
}

static void
web_io_drain_frame_output (struct web_async_state *state)
{
  pthread_mutex_lock (&state->frame_output.mutex);
  if (state->frame_output.frame_ready)
    {
      struct web_frame_buffer *buffer
	= &state->frame_output.buffers[state->frame_output.ready_idx];
      web_pending_append (state, buffer->data, buffer->len);
      state->frame_output.frame_ready = false;
    }
  pthread_mutex_unlock (&state->frame_output.mutex);
}

static void *
web_io_thread_func (void *arg)
{
  struct web_async_state *state = arg;

  sigset_t all;
  sigfillset (&all);
  pthread_sigmask (SIG_BLOCK, &all, NULL);

  while (state->io_running)
    {
      struct pollfd fds[2];
      fds[0].fd = state->proxy_fd;
      fds[0].events = POLLIN;
      if (state->pending_write_pos < state->pending_write_len)
	fds[0].events |= POLLOUT;
      fds[0].revents = 0;

      fds[1].fd = state->frame_wake_pipe[0];
      fds[1].events = POLLIN;
      fds[1].revents = 0;

      int n = poll (fds, 2, WEB_POLL_TIMEOUT_MS);
      if (n < 0 && errno == EINTR)
	continue;
      if (n < 0)
	{
	  state->io_running = false;
	  break;
	}

      if (n > 0 && (fds[0].revents & POLLIN))
	{
	  web_io_read_proxy (state);
	  web_io_parse_events (state);
	}

      /* Drain the wake pipe — just discard the bytes.  */
      if (n > 0 && (fds[1].revents & POLLIN))
	{
	  char drain[64];
	  while (read (state->frame_wake_pipe[0], drain, sizeof drain) > 0)
	    ;
	}

      web_io_drain_control_output (state);
      web_io_drain_frame_output (state);
      web_io_maybe_send_heartbeat (state);
      web_pending_flush (state);

      if (n > 0 && (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)))
	state->io_running = false;
    }

  return NULL;
}

int
web_async_init (struct web_async_state *state, int proxy_fd)
{
  memset (state, 0, sizeof *state);
  state->proxy_fd = proxy_fd;
  state->notify_pipe[0] = state->notify_pipe[1] = -1;
  state->frame_wake_pipe[0] = state->frame_wake_pipe[1] = -1;
  bool queue_initialized = false;
  bool frame_output_initialized = false;
  bool control_mutex_initialized = false;

  if (pipe (state->notify_pipe) < 0)
    return -1;
  if (pipe (state->frame_wake_pipe) < 0)
    goto fail;
  if (web_set_nonblocking (state->notify_pipe[0]) < 0
      || web_set_nonblocking (state->notify_pipe[1]) < 0
      || web_set_nonblocking (state->frame_wake_pipe[0]) < 0
      || web_set_nonblocking (state->frame_wake_pipe[1]) < 0
      || web_set_nonblocking (state->proxy_fd) < 0)
    goto fail;

  web_event_queue_init (&state->input_queue);
  queue_initialized = true;
  web_frame_output_init (&state->frame_output);
  frame_output_initialized = true;
  pthread_mutex_init (&state->control_mutex, NULL);
  control_mutex_initialized = true;

  state->io_read_capacity = WEB_IO_READ_INIT_SIZE;
  state->io_read_buf = malloc (state->io_read_capacity);
  if (!state->io_read_buf)
    goto fail;

  clock_gettime (CLOCK_MONOTONIC, &state->last_heartbeat);
  state->last_yield_time = state->last_heartbeat;
  return 0;

 fail:
  if (control_mutex_initialized)
    pthread_mutex_destroy (&state->control_mutex);
  if (frame_output_initialized)
    web_frame_output_destroy (&state->frame_output);
  if (queue_initialized)
    web_event_queue_destroy (&state->input_queue);
  for (int i = 0; i < 2; ++i)
    {
      if (state->notify_pipe[i] >= 0)
	{
	  close (state->notify_pipe[i]);
	  state->notify_pipe[i] = -1;
	}
      if (state->frame_wake_pipe[i] >= 0)
	{
	  close (state->frame_wake_pipe[i]);
	  state->frame_wake_pipe[i] = -1;
	}
    }
  free (state->io_read_buf);
  state->io_read_buf = NULL;
  return -1;
}

int
web_async_start (struct web_async_state *state)
{
  state->io_running = true;
  if (pthread_create (&state->io_thread, NULL, web_io_thread_func, state) != 0)
    {
      state->io_running = false;
      return -1;
    }

  state->io_started = true;
  return 0;
}

void
web_async_shutdown (struct web_async_state *state)
{
  state->io_running = false;
  if (state->proxy_fd >= 0)
    shutdown (state->proxy_fd, SHUT_RDWR);

  if (state->io_started)
    {
      pthread_join (state->io_thread, NULL);
      state->io_started = false;
    }

  if (state->proxy_fd >= 0)
    {
      close (state->proxy_fd);
      state->proxy_fd = -1;
    }

  for (int i = 0; i < 2; ++i)
    {
      if (state->notify_pipe[i] >= 0)
	{
	  close (state->notify_pipe[i]);
	  state->notify_pipe[i] = -1;
	}
      if (state->frame_wake_pipe[i] >= 0)
	{
	  close (state->frame_wake_pipe[i]);
	  state->frame_wake_pipe[i] = -1;
	}
    }

  web_event_queue_destroy (&state->input_queue);
  web_frame_output_destroy (&state->frame_output);
  pthread_mutex_destroy (&state->control_mutex);

  free (state->io_read_buf);
  free (state->control_buf);
  free (state->pending_write_buf);
  memset (state, 0, sizeof *state);
  state->proxy_fd = -1;
  state->notify_pipe[0] = state->notify_pipe[1] = -1;
  state->frame_wake_pipe[0] = state->frame_wake_pipe[1] = -1;
}

#endif /* HAVE_PTHREAD */
