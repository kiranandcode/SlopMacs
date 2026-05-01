/* Web display backend for GNU Emacs.

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

#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "lisp.h"
#include "blockinput.h"
#include "coding.h"
#include "frame.h"
#include "termhooks.h"
#include "keyboard.h"
#include "buffer.h"
#include "window.h"
#include "termchar.h"
#include "webterm.h"
#include "webgui.h"
#include "dispextern.h"
#include "process.h"
#include "menu.h"
#include "atimer.h"

/* The single display info for this web terminal.  */
struct web_display_info *web_display_info;

/* Linked list of all web displays, used by frame.c via x_display_list.  */
struct web_display_info *x_display_list;


/* Modifier bits in wire protocol (must match input.js).  */
#define WIRE_MOD_SHIFT (1 << 0)
#define WIRE_MOD_CTRL  (1 << 2)
#define WIRE_MOD_META  (1 << 3)
#define WIRE_MOD_SUPER (1 << 4)


/* ====================================================================
   Write buffer — batch NDJSON lines, flush to proxy_fd.
   ==================================================================== */

#define WRITE_BUF_INIT_SIZE (64 * 1024)

static void
web_write_ensure (struct web_display_info *dpyinfo, int need)
{
  if (dpyinfo->write_buf_len + need > dpyinfo->write_buf_size)
    {
      int newsize = dpyinfo->write_buf_size * 2;
      if (newsize < dpyinfo->write_buf_len + need)
	newsize = dpyinfo->write_buf_len + need + WRITE_BUF_INIT_SIZE;
      dpyinfo->write_buf = xrealloc (dpyinfo->write_buf, newsize);
      dpyinfo->write_buf_size = newsize;
    }
}

/* Append a string to the write buffer.  */
static void
web_write_str (struct web_display_info *dpyinfo, const char *s, int len)
{
  web_write_ensure (dpyinfo, len);
  memcpy (dpyinfo->write_buf + dpyinfo->write_buf_len, s, len);
  dpyinfo->write_buf_len += len;
}

/* Write a string literal — sizeof computes length at compile time,
   eliminating manual byte-counting bugs.  */
#define WR_LIT(dp, s) web_write_str (dp, s, sizeof (s) - 1)

/* Append a printf-formatted string to the write buffer.  */
static void
web_write_printf (struct web_display_info *dpyinfo, const char *fmt, ...)
{
  char buf[4096];
  va_list ap;
  va_start (ap, fmt);
  int n = vsnprintf (buf, sizeof buf, fmt, ap);
  va_end (ap);
  if (n > 0)
    web_write_str (dpyinfo, buf, n);
}

/* Flush the write buffer to the proxy fd.  */
static void
web_write_flush (struct web_display_info *dpyinfo)
{
  if (dpyinfo->proxy_fd < 0 || dpyinfo->write_buf_len == 0)
    return;

  int written = 0;
  int retries = 0;
  while (written < dpyinfo->write_buf_len)
    {
      ssize_t n = write (dpyinfo->proxy_fd,
			 dpyinfo->write_buf + written,
			 dpyinfo->write_buf_len - written);
      if (n < 0)
	{
	  if (errno == EINTR)
	    continue;
	  if (errno == EAGAIN || errno == EWOULDBLOCK)
	    {
	      struct timespec ts = { 0, 1000000 }; /* 1ms */
	      nanosleep (&ts, NULL);
	      if (++retries > 100)
		break; /* give up after ~100ms */
	      continue;
	    }
	  /* Proxy died.  */
	  break;
	}
      written += n;
      retries = 0;
    }
  dpyinfo->write_buf_len = 0;
}

static void
web_control_flush (struct web_display_info *dpyinfo)
{
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      web_async_enqueue_output (&dpyinfo->async,
				dpyinfo->write_buf, dpyinfo->write_buf_len);
      dpyinfo->write_buf_len = 0;
      return;
    }
#endif

  web_write_flush (dpyinfo);
}


/* ====================================================================
   JSON text escaping helper.
   ==================================================================== */

/* Append JSON-escaped text to the write buffer.  Handles ", \, and
   control characters.  */
static void
web_write_json_string (struct web_display_info *dpyinfo,
		       const char *s, int len)
{
  WR_LIT (dpyinfo, "\"");
  for (int i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char) s[i];
      switch (c)
	{
	case '"':
	  WR_LIT (dpyinfo, "\\\"");
	  break;
	case '\\':
	  WR_LIT (dpyinfo, "\\\\");
	  break;
	case '\n':
	  WR_LIT (dpyinfo, "\\n");
	  break;
	case '\r':
	  WR_LIT (dpyinfo, "\\r");
	  break;
	case '\t':
	  WR_LIT (dpyinfo, "\\t");
	  break;
	default:
	  if (c < 0x20)
	    {
	      char esc[8];
	      int n = snprintf (esc, sizeof esc, "\\u%04x", c);
	      web_write_str (dpyinfo, esc, n);
	    }
	  else
	    web_write_str (dpyinfo, (const char *)&s[i], 1);
	  break;
	}
    }
  WR_LIT (dpyinfo, "\"");
}

/* Format a pixel color as a "#RRGGBB" string.  */
static void
color_to_hex (unsigned long pixel, char *buf)
{
  snprintf (buf, 8, "#%06lx", pixel & 0xFFFFFF);
}


/* ====================================================================
   JSON frame state helpers.
   ==================================================================== */

/* Register a face_id for inclusion in the faces dictionary.  */
static void
json_register_face (struct json_frame_state *js, int face_id)
{
  for (int i = 0; i < js->nface_ids; i++)
    if (js->face_ids[i] == face_id)
      return;
  if (js->nface_ids < 512)
    js->face_ids[js->nface_ids++] = face_id;
}

/* Find or create a json_window for the given window id.  */
static struct json_window *
json_get_window (struct json_frame_state *js, EMACS_INT id)
{
  for (int i = 0; i < js->nwindows; i++)
    if (js->windows[i].id == id)
      return &js->windows[i];

  if (js->nwindows >= 8)
    return &js->windows[0]; /* fallback */

  struct json_window *jw = &js->windows[js->nwindows++];
  memset (jw, 0, sizeof *jw);
  jw->id = id;
  jw->active = true;
  return jw;
}

/* Find or create a json_line for the given row in a window.  */
static struct json_line *
json_get_line (struct json_window *jw, int row_index)
{
  for (int i = 0; i < jw->nlines; i++)
    if (jw->lines[i].row_index == row_index)
      return &jw->lines[i];

  if (jw->nlines >= 80)
    return &jw->lines[0]; /* fallback */

  struct json_line *jl = &jw->lines[jw->nlines++];
  memset (jl, 0, sizeof *jl);
  jl->row_index = row_index;
  return jl;
}

static int
web_glyph_row_index (struct window *w, struct glyph_row *row,
		     int frame_y_fallback)
{
  struct frame *f = XFRAME (w->frame);
  int ch = FRAME_LINE_HEIGHT (f);
  int y;

  if (ch <= 0)
    {
      struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
      ch = dpyinfo->default_char_height;
    }

  if (row)
    y = row->y;
  else
    y = frame_y_fallback - WINDOW_TO_FRAME_PIXEL_Y (w, 0);

  if (ch <= 0)
    return 0;
  if (WINDOW_TOTAL_LINES (w) <= 0)
    return 0;

  int row_index = y / ch;
  if (row_index < 0)
    row_index = 0;
  if (row_index >= WINDOW_TOTAL_LINES (w))
    row_index = WINDOW_TOTAL_LINES (w) - 1;

  return row_index;
}

/* Collect all leaf window IDs on a frame by walking the window tree.
   Used to tell the browser which windows are still alive so it can
   prune stale windows after delete-window, etc.  */
static void
web_collect_live_windows (Lisp_Object window,
			  EMACS_INT *ids, int *count, int max)
{
  while (!NILP (window) && WINDOWP (window))
    {
      struct window *w = XWINDOW (window);
      if (WINDOW_LEAF_P (w))
	{
	  if (*count < max)
	    ids[(*count)++] = w->sequence_number;
	}
      else if (WINDOWP (w->contents))
	web_collect_live_windows (w->contents, ids, count, max);
      window = w->next;
    }
}

/* Reset the json frame state for the next redisplay cycle.
   Only zero the counters — the array payloads are overwritten
   before use, so clearing them would be needless O(n) work.  */
static void
json_state_reset (struct json_frame_state *js)
{
  js->nwindows = 0;
  js->nface_ids = 0;
  js->nscrolls = 0;
  js->clear_pending = false;
  js->clear_bg = 0;
  js->current_window = NULL;
  js->current_line = NULL;
  js->current_line_row = -1;
}


/* ====================================================================
   RIF functions (redisplay interface) — JSON accumulation.
   ==================================================================== */

/* Encode a single Unicode codepoint as UTF-8.  Return bytes written.  */
static int
encode_utf8 (unsigned int cp, unsigned char *buf)
{
  if (cp < 0x80)
    { buf[0] = cp; return 1; }
  else if (cp < 0x800)
    { buf[0] = 0xC0 | (cp >> 6);
      buf[1] = 0x80 | (cp & 0x3F);
      return 2; }
  else if (cp < 0x10000)
    { buf[0] = 0xE0 | (cp >> 12);
      buf[1] = 0x80 | ((cp >> 6) & 0x3F);
      buf[2] = 0x80 | (cp & 0x3F);
      return 3; }
  else
    { buf[0] = 0xF0 | (cp >> 18);
      buf[1] = 0x80 | ((cp >> 12) & 0x3F);
      buf[2] = 0x80 | ((cp >> 6) & 0x3F);
      buf[3] = 0x80 | (cp & 0x3F);
      return 4; }
}

static void
web_update_window_begin (struct window *w)
{
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;

  /* Find or allocate a json_window slot.  */
  struct json_window *jw = json_get_window (js, w->sequence_number);
  jw->x = WINDOW_LEFT_EDGE_COL (w);
  jw->y = WINDOW_TOP_EDGE_LINE (w);
  jw->w = WINDOW_TOTAL_COLS (w);
  jw->h = WINDOW_TOTAL_LINES (w);
  jw->active = true;
  jw->is_menu_bar = WINDOW_MENU_BAR_P (w);

  js->current_window = jw;
  js->current_line = NULL;
  js->current_line_row = -1;

}

static void
web_update_window_end (struct window *w, bool cursor_on_p,
		       bool mouse_face_overwritten_p)
{
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;

  js->current_window = NULL;
  js->current_line = NULL;
  js->current_line_row = -1;
}

/* Build complete line content from a glyph row into json_state.
   Called from both draw_glyph_string and after_update_window_line
   to ensure we capture content regardless of which redisplay path
   Emacs uses.  */
static void
web_build_row_content (struct window *w, struct glyph_row *row)
{
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;
  struct json_window *jw = js->current_window;

  if (!row)
    return;

  /* If called outside update_window_begin/end (e.g. cursor drawing),
     look up or create the window slot directly.  */
  if (!jw || jw->id != w->sequence_number)
    {
      jw = json_get_window (js, w->sequence_number);
      jw->x = WINDOW_LEFT_EDGE_COL (w);
      jw->y = WINDOW_TOP_EDGE_LINE (w);
      jw->w = WINDOW_TOTAL_COLS (w);
      jw->h = WINDOW_TOTAL_LINES (w);
      jw->active = true;
    }

  int row_index = web_glyph_row_index (w, row,
                                       WINDOW_TO_FRAME_PIXEL_Y (w, row->y));

  struct json_line *jl = json_get_line (jw, row_index);

  /* Skip if this row was already built in this redisplay cycle.  */
  if (jl->complete)
    return;

  /* Read the COMPLETE row from the glyph matrix.  */
  jl->nruns = 0;

  struct glyph *glyph = row->glyphs[TEXT_AREA];
  struct glyph *end = glyph + row->used[TEXT_AREA];

  int current_face = -1;
  struct json_run *run = NULL;

  for (; glyph < end; glyph++)
    {
      int face_id = glyph->face_id;

      switch (glyph->type)
	{
	case CHAR_GLYPH:
	case COMPOSITE_GLYPH:
	case GLYPHLESS_GLYPH:
	  {
	    unsigned int c = glyph->u.ch;
	    if (c == 0)
	      c = ' ';

	    if (face_id != current_face || !run)
	      {
		if (jl->nruns >= 32)
		  goto done;
		run = &jl->runs[jl->nruns++];
		run->face_id = face_id;
		run->text_len = 0;
		current_face = face_id;
		json_register_face (js, face_id);
	      }

	    if (run->text_len < 1020)
	      {
		int n = encode_utf8 (c,
				     (unsigned char *) run->text
				     + run->text_len);
		run->text_len += n;
	      }
	    break;
	  }

	case STRETCH_GLYPH:
	  {
	    int cw = dpyinfo->default_char_width;
	    int ncells = cw > 0
	      ? (glyph->pixel_width + cw - 1) / cw : 1;

	    if (face_id != current_face || !run)
	      {
		if (jl->nruns >= 32)
		  goto done;
		run = &jl->runs[jl->nruns++];
		run->face_id = face_id;
		run->text_len = 0;
		current_face = face_id;
		json_register_face (js, face_id);
	      }

	    for (int i = 0; i < ncells && run->text_len < 4090; i++)
	      run->text[run->text_len++] = ' ';
	    break;
	  }

	case IMAGE_GLYPH:
	  {
	    int cw = dpyinfo->default_char_width;
	    int ncells = cw > 0
	      ? (glyph->pixel_width + cw - 1) / cw : 1;

	    if (jl->nruns >= 32)
	      goto done;
	    run = &jl->runs[jl->nruns++];
	    run->face_id = face_id;
	    run->text_len = 0;
	    current_face = -1;
	    json_register_face (js, face_id);

	    for (int i = 0; i < ncells && run->text_len < 4090; i++)
	      run->text[run->text_len++] = ' ';
	    break;
	  }

	default:
	  break;
	}
    }

 done:
  jl->mode_line_p = row->mode_line_p;
  jl->continued_p = row->continued_p;
  jl->truncated_left_p = row->truncated_on_left_p;
  jl->truncated_right_p = row->truncated_on_right_p;
  jl->complete = true;
  jw->has_complete_lines = true;
}

static void
web_draw_glyph_string (struct glyph_string *s)
{
  /* Capture the complete row content.  Emacs sometimes redraws rows
     via draw_glyph_string without calling after_update_window_line
     (e.g. cursor updates, expose events).  We must capture content
     here to handle those paths.
     web_build_row_content skips rows already marked complete in this
     cycle, so repeated calls for the same row are cheap.  */
  if (s->row && s->w)
    web_build_row_content (s->w, s->row);
}

static void
web_scroll_run (struct window *w, struct run *run)
{
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;

  int ch = dpyinfo->default_char_height;
  if (ch <= 0)
    return;

  int delta_rows = (run->desired_y - run->current_y) / ch;
  if (delta_rows == 0)
    return;

  if (js->nscrolls < 16)
    {
      js->scrolls[js->nscrolls].window_id = w->sequence_number;
      js->scrolls[js->nscrolls].delta_rows = delta_rows;
      js->nscrolls++;
    }
}

static void
web_after_update_window_line (struct window *w,
			      struct glyph_row *desired_row)
{
  /* Force rebuild even if draw_glyph_string already captured this row,
     since after_update_window_line has the final desired_row data.  */
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;
  struct json_window *jw = js->current_window;
  if (jw && desired_row)
    {
      int ri = web_glyph_row_index (w, desired_row,
				    WINDOW_TO_FRAME_PIXEL_Y (w,
							     desired_row->y));
      struct json_line *jl = json_get_line (jw, ri);
      jl->complete = false; /* Clear so web_build_row_content re-reads.  */
    }
  web_build_row_content (w, desired_row);
}

static void
web_flush_display (struct frame *f)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;

#ifdef HAVE_PTHREAD
  if (!dpyinfo->async_enabled && dpyinfo->proxy_fd < 0)
    return;
#else
  if (dpyinfo->proxy_fd < 0)
    return;
#endif

  /* Skip empty flushes — nothing changed, don't waste bandwidth.  */
  bool has_data = js->clear_pending || js->nscrolls > 0;
  if (!has_data)
    {
      for (int i = 0; i < js->nwindows; i++)
	if (js->windows[i].has_complete_lines || js->windows[i].has_cursor)
	  { has_data = true; break; }
    }
  if (!has_data)
    {
      json_state_reset (js);
      return;
    }

  /* 1. Clear frame.  */
  if (js->clear_pending)
    {
      char bg[8];
      color_to_hex (js->clear_bg, bg);
      web_write_printf (dpyinfo,
			"{\"type\":\"clear_frame\",\"bg\":\"%s\"}\n", bg);
    }

  /* 2. Scroll events.  */
  for (int i = 0; i < js->nscrolls; i++)
    web_write_printf (dpyinfo,
		      "{\"type\":\"scroll\",\"window_id\":%ld,"
		      "\"delta_rows\":%d}\n",
		      (long)js->scrolls[i].window_id,
		      js->scrolls[i].delta_rows);

  /* 3. Build frame_update JSON if we have any data.  */
  if (js->nwindows > 0 || js->nface_ids > 0)
    {
      {
	struct timespec _ts = current_timespec ();
	uint64_t _ms = (uint64_t)_ts.tv_sec * 1000 + _ts.tv_nsec / 1000000;
	web_write_printf (dpyinfo,
			  "{\"type\":\"frame_update\",\"ts\":%llu",
			  (unsigned long long)_ms);
      }

      /* 3a. Faces dictionary.  */
      if (js->nface_ids > 0)
	{
	  WR_LIT (dpyinfo, ",\"faces\":{");
	  bool first_face = true;
	  for (int i = 0; i < js->nface_ids; i++)
	    {
	      int fid = js->face_ids[i];
	      struct face *face = NULL;
	      int real_fid = fid & 0xFFFFF; /* strip synthetic bits */
	      unsigned long fg, bg;
	      bool is_cursor = (fid & 0x100000) != 0;
	      bool is_inverse = (fid & 0x200000) != 0;

	      face = FACE_FROM_ID_OR_NULL (f, real_fid);
	      if (!face)
		face = FACE_FROM_ID_OR_NULL (f, DEFAULT_FACE_ID);
	      if (!face)
		continue;

	      if (is_cursor)
		{
		  fg = FRAME_X_OUTPUT (f)->cursor_foreground_color;
		  bg = FRAME_CURSOR_COLOR (f);
		}
	      else if (is_inverse)
		{
		  fg = face->background;
		  bg = face->foreground;
		}
	      else
		{
		  fg = face->foreground;
		  bg = face->background;
		}

	      char fg_hex[8], bg_hex[8];
	      color_to_hex (fg, fg_hex);
	      color_to_hex (bg, bg_hex);

	      if (!first_face)
		WR_LIT (dpyinfo, ",");
	      first_face = false;

	      web_write_printf (dpyinfo, "\"%d\":{\"fg\":\"%s\",\"bg\":\"%s\"",
				fid, fg_hex, bg_hex);

	      /* Style flags from face font properties.  */
	      if (face->font)
		{
		  struct font *font = face->font;
		  Lisp_Object weight = font->props[FONT_WEIGHT_INDEX];
		  if (FIXNUMP (weight) && XFIXNUM (weight) >= 200)
		    WR_LIT (dpyinfo, ",\"bold\":true");

		  Lisp_Object slant = font->props[FONT_SLANT_INDEX];
		  if (FIXNUMP (slant) && XFIXNUM (slant) >= 200)
		    WR_LIT (dpyinfo, ",\"italic\":true");
		}

	      if (face->underline != FACE_NO_UNDERLINE)
		WR_LIT (dpyinfo, ",\"underline\":true");
	      if (face->strike_through_p)
		WR_LIT (dpyinfo, ",\"strike\":true");
	      if (face->box == FACE_RAISED_BOX)
		WR_LIT (dpyinfo, ",\"box\":\"raised\"");
	      else if (face->box == FACE_SUNKEN_BOX)
		WR_LIT (dpyinfo, ",\"box\":\"sunken\"");
	      else if (face->box != FACE_NO_BOX)
		WR_LIT (dpyinfo, ",\"box\":\"flat\"");

	      WR_LIT (dpyinfo, "}");
	    }
	  WR_LIT (dpyinfo, "}");
	}

      /* 3b. Windows array — skip windows with no dirty data.  */
      if (js->nwindows > 0)
	{
	  WR_LIT (dpyinfo, ",\"windows\":[");
	  bool first_win = true;
	  for (int wi = 0; wi < js->nwindows; wi++)
	    {
	      struct json_window *jw = &js->windows[wi];
	      if (!jw->has_complete_lines && !jw->has_cursor)
		continue;
	      if (!first_win)
		WR_LIT (dpyinfo, ",");
	      first_win = false;

	      web_write_printf (dpyinfo,
				"{\"id\":%ld,\"x\":%d,\"y\":%d,"
				"\"w\":%d,\"h\":%d",
				(long)jw->id, jw->x, jw->y,
				jw->w, jw->h);

	      if (jw->is_menu_bar)
		WR_LIT (dpyinfo, ",\"menu_bar\":true");

	      /* Lines — only send complete (fully built) lines.  */
	      if (jw->nlines > 0)
		{
		  WR_LIT (dpyinfo, ",\"lines\":[");
		  bool first_line = true;
		  for (int li = 0; li < jw->nlines; li++)
		    {
		      struct json_line *jl = &jw->lines[li];
		      if (!jl->complete)
			continue;
		      if (!first_line)
			WR_LIT (dpyinfo, ",");
		      first_line = false;

		      web_write_printf (dpyinfo,
					"{\"row\":%d,\"mode_line\":%s,"
					"\"continued\":%s",
					jl->row_index,
					jl->mode_line_p ? "true" : "false",
					jl->continued_p ? "true" : "false");

		      /* Runs.  */
		      if (jl->nruns > 0)
			{
			  WR_LIT (dpyinfo, ",\"runs\":[");
			  for (int ri = 0; ri < jl->nruns; ri++)
			    {
			      struct json_run *run = &jl->runs[ri];
			      if (ri > 0)
				WR_LIT (dpyinfo, ",");
			      web_write_printf (dpyinfo,
						"{\"face_id\":%d,\"text\":",
						run->face_id);
			      web_write_json_string (dpyinfo, run->text,
						     run->text_len);
			      WR_LIT (dpyinfo, "}");
			    }
			  WR_LIT (dpyinfo, "]");
			}

		      WR_LIT (dpyinfo, "}");
		    }
		  WR_LIT (dpyinfo, "]");
		}

	      /* Cursor.  */
	      if (jw->has_cursor)
		web_write_printf (dpyinfo,
				  ",\"cursor\":{\"row\":%d,\"col\":%d,"
				  "\"type\":%d,\"active\":%s}",
				  jw->cursor.row, jw->cursor.col,
				  jw->cursor.type,
				  jw->cursor.active ? "true" : "false");

	      WR_LIT (dpyinfo, "}");
	    }
	  WR_LIT (dpyinfo, "]");
	}

      /* 3c. List of all live window IDs so the browser can
	 prune windows that were deleted.  */
      if (f)
	{
	  EMACS_INT live_ids[32];
	  int nlive = 0;
	  web_collect_live_windows (f->root_window,
				   live_ids, &nlive, 32);
	  WR_LIT (dpyinfo, ",\"all_windows\":[");
	  for (int i = 0; i < nlive; i++)
	    {
	      if (i > 0)
		WR_LIT (dpyinfo, ",");
	      web_write_printf (dpyinfo, "%ld", (long)live_ids[i]);
	    }
	  WR_LIT (dpyinfo, "]");
	}

      WR_LIT (dpyinfo, "}\n");
    }

  /* 4. Flush and reset.  */
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      web_frame_output_write (&dpyinfo->async.frame_output,
			      dpyinfo->write_buf, dpyinfo->write_buf_len);
      dpyinfo->write_buf_len = 0;
      /* Wake the I/O thread immediately so it sends the frame
	 without waiting for the next poll timeout.  */
      web_frame_output_wake (&dpyinfo->async);
    }
  else
#endif
    web_write_flush (dpyinfo);
  json_state_reset (js);
}

static void
web_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			struct draw_fringe_bitmap_params *p)
{
  /* Fringes are handled by CSS margins; no-op.  */
}

static void
web_define_fringe_bitmap (int which, unsigned short *bits,
			  int h, int wd)
{
  /* No-op: we don't pre-register fringe bitmaps.  */
}

static void
web_destroy_fringe_bitmap (int which)
{
  /* No-op.  */
}

static void
web_compute_glyph_string_overhangs (struct glyph_string *s)
{
  /* Monospace font: no overhang.  */
  s->left_overhang = 0;
  s->right_overhang = 0;
}

static void
web_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
  /* No-op: browser handles cursor shape.  */
}

static void
web_clear_frame_area (struct frame *f, int x, int y,
		      int width, int height)
{
  /* In JSON mode, clearing is done implicitly — browser renders
     empty lines with default background.  No-op here.  */
}

static void
web_clear_under_internal_border (struct frame *f)
{
  /* Browser handles borders via CSS.  No-op.  */
}

static void
web_draw_window_cursor (struct window *w,
			struct glyph_row *glyph_row,
			int x, int y,
			enum text_cursor_kinds cursor_type,
			int cursor_width, bool on_p, bool active_p)
{
  struct frame *f = XFRAME (w->frame);
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;
  struct json_window *jw = js->current_window;

  if (!on_p)
    return;

  /* If called outside update_window_begin/end, look up the window.  */
  if (!jw || jw->id != w->sequence_number)
    {
      jw = json_get_window (js, w->sequence_number);
      jw->x = WINDOW_LEFT_EDGE_COL (w);
      jw->y = WINDOW_TOP_EDGE_LINE (w);
      jw->w = WINDOW_TOTAL_COLS (w);
      jw->h = WINDOW_TOTAL_LINES (w);
      jw->active = true;
    }

  w->phys_cursor_type = cursor_type;
  w->phys_cursor_on_p = true;
  w->phys_cursor_width = cursor_width;

  /* Compute cursor position in character cells.  */
  int cw = dpyinfo->default_char_width;
  int col = cw > 0 ? (x - WINDOW_TEXT_TO_FRAME_PIXEL_X (w, 0)) / cw : 0;
  int row = web_glyph_row_index (w, glyph_row, y);

  jw->has_cursor = true;
  jw->cursor.row = row;
  jw->cursor.col = col;
  jw->cursor.active = active_p;

  /* Only record cursor position and type — the browser renders
     the cursor via CSS.  We do NOT call draw_phys_cursor_glyph
     because it would add a single-char glyph_string to the
     json_line, which during cursor-blink redraws would create
     a partial line that overwrites the full line data in the
     browser.  */
  switch (cursor_type)
    {
    case FILLED_BOX_CURSOR:
      jw->cursor.type = 0; /* box */
      break;
    case HOLLOW_BOX_CURSOR:
      jw->cursor.type = 1; /* hollow */
      break;
    case BAR_CURSOR:
      jw->cursor.type = 2; /* bar */
      break;
    case HBAR_CURSOR:
      jw->cursor.type = 3; /* hbar */
      break;
    case NO_CURSOR:
      jw->has_cursor = false;
      w->phys_cursor_width = 0;
      break;
    default:
      jw->cursor.type = 0;
      /* Browser renders cursor via CSS — no draw_phys_cursor_glyph
	 which would overwrite line data.  */
      break;
    }
}

static void
web_draw_vertical_window_border (struct window *w,
				 int x, int y_0, int y_1)
{
  /* Browser renders borders via CSS between window divs.  No-op.  */
}

static void
web_draw_window_divider (struct window *w,
			 int x_0, int x_1, int y_0, int y_1)
{
  /* Browser renders dividers via CSS.  No-op.  */
}

static void
web_show_hourglass (struct frame *f)
{
  /* No-op.  */
}

static void
web_hide_hourglass (struct frame *f)
{
  /* No-op.  */
}

static void
web_default_font_parameter (struct frame *f, Lisp_Object parms)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Lisp_Object font_param = gui_display_get_arg (dpyinfo, parms,
						 Qfont, NULL, NULL,
						 RES_TYPE_STRING);
  Lisp_Object font = Qnil;

  if (BASE_EQ (font_param, Qunbound))
    font_param = Qnil;

  if (!NILP (font_param))
    font = font_open_by_name (f, font_param);

  if (NILP (font))
    {
      font = (!NILP (font_param)
	      ? font_param
	      : gui_display_get_arg (dpyinfo, parms, Qfont,
				     "font", "Font",
				     RES_TYPE_STRING));
    }

  if (!FONTP (font) && !STRINGP (font))
    {
      static const char *try_fonts[] = {
	"monospace-12", "Monospace-12", "Courier-12", NULL
      };
      for (int i = 0; try_fonts[i]; i++)
	{
	  font = font_open_by_name (f, build_unibyte_string (try_fonts[i]));
	  if (!NILP (font))
	    break;
	}
      if (NILP (font))
	error ("No suitable font was found");
    }

  gui_default_parameter (f, parms, Qfont, font, "font", "Font",
			 RES_TYPE_STRING);
}


/* The redisplay interface structure for the web backend.  */

static struct redisplay_interface web_redisplay_interface = {
  web_frame_parm_handlers,
  gui_produce_glyphs,
  gui_write_glyphs,
  gui_insert_glyphs,
  gui_clear_end_of_line,
  web_scroll_run,
  web_after_update_window_line,
  web_update_window_begin,
  web_update_window_end,
  web_flush_display,
  gui_clear_window_mouse_face,
  gui_get_glyph_overhangs,
  gui_fix_overlapping_area,
  web_draw_fringe_bitmap,
  web_define_fringe_bitmap,
  web_destroy_fringe_bitmap,
  web_compute_glyph_string_overhangs,
  web_draw_glyph_string,
  web_define_frame_cursor,
  web_clear_frame_area,
  web_clear_under_internal_border,
  web_draw_window_cursor,
  web_draw_vertical_window_border,
  web_draw_window_divider,
  NULL, /* shift_glyphs_for_insert */
  web_show_hourglass,
  web_hide_hourglass,
  web_default_font_parameter,
};


/* ====================================================================
   Terminal hooks.
   ==================================================================== */

static void
web_clear_frame (struct frame *f)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct json_frame_state *js = &dpyinfo->json_state;
  js->clear_pending = true;
  js->clear_bg = FRAME_BACKGROUND_COLOR (f);
}

static void
web_ring_bell (struct frame *f)
{
  /* Could send a visual bell command.  */
}

static void
web_toggle_invisible_pointer (struct frame *f, bool invisible)
{
  /* No-op.  */
}

static void
web_update_begin (struct frame *f)
{
  /* Nothing special needed.  */
}

static void
web_update_end (struct frame *f)
{
  /* Flush after update cycle completes.  */
  web_flush_display (f);
}


/* ====================================================================
   Input event reading — proxy_fd → Emacs input events (NDJSON).
   ==================================================================== */

/* Return the first frame on this display, or NULL.  */
static struct frame *
web_any_frame (struct web_display_info *dpyinfo)
{
  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WEB_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo)
	return f;
    }
  return NULL;
}

/* Simple JSON field extraction helpers.
   These search for "key":value patterns in a JSON string.  */

/* Find "key": and return pointer to the value start, or NULL.  */
static const char *
json_find_key (const char *json, int json_len, const char *key)
{
  int klen = strlen (key);
  /* Search for "key": */
  for (int i = 0; i < json_len - klen - 3; i++)
    {
      if (json[i] == '"'
	  && memcmp (json + i + 1, key, klen) == 0
	  && json[i + 1 + klen] == '"'
	  && json[i + 2 + klen] == ':')
	return json + i + 3 + klen;
    }
  return NULL;
}

static int
json_extract_int (const char *json, int json_len, const char *key)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return 0;
  /* Skip whitespace.  */
  while (*v == ' ') v++;
  return atoi (v);
}

static bool
json_extract_bool (const char *json, int json_len, const char *key)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return false;
  while (*v == ' ') v++;
  return (*v == 't'); /* "true" */
}

/* Extract a string value into buf (up to buf_size-1 chars).
   Returns length, or -1 if not found.  */
static int
json_extract_string (const char *json, int json_len, const char *key,
		     char *buf, int buf_size)
{
  const char *v = json_find_key (json, json_len, key);
  if (!v)
    return -1;
  while (*v == ' ') v++;
  if (*v != '"')
    return -1;
  v++; /* skip opening quote */
  int len = 0;
  while (*v && *v != '"' && len < buf_size - 1)
    {
      if (*v == '\\' && *(v+1))
	{
	  v++;
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
      v++;
    }
  buf[len] = '\0';
  return len;
}

/* Check if json contains "type":"value".  */
static bool
json_type_is (const char *json, int json_len, const char *type_value)
{
  char buf[64];
  int len = json_extract_string (json, json_len, "type", buf, sizeof buf);
  if (len < 0)
    return false;
  return strcmp (buf, type_value) == 0;
}

#ifdef HAVE_PTHREAD
static int
web_dispatch_event (struct web_display_info *dpyinfo, struct web_event *event,
		    struct input_event *hold_quit)
{
  (void) hold_quit;

  struct frame *f = web_any_frame (dpyinfo);
  if (!f)
    return 0;

  if (!dpyinfo->x_focus_frame)
    dpyinfo->x_focus_frame = f;

  Lisp_Object frame;
  XSETFRAME (frame, f);

  switch (event->type)
    {
    case WEB_EVT_KEY:
      {
	struct input_event ie;
	EVENT_INIT (ie);
	ie.frame_or_window = frame;

	int emacs_mods = 0;
	if (event->mods & WIRE_MOD_SHIFT) emacs_mods |= shift_modifier;
	if (event->mods & WIRE_MOD_CTRL)  emacs_mods |= ctrl_modifier;
	if (event->mods & WIRE_MOD_META)  emacs_mods |= meta_modifier;
	if (event->mods & WIRE_MOD_SUPER) emacs_mods |= super_modifier;

	if (event->keycode >= 0xFF00)
	  {
	    ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	    ie.code = event->keycode;
	  }
	else if (event->character > 0 && event->character < 128)
	  {
	    ie.kind = ASCII_KEYSTROKE_EVENT;
	    ie.code = event->character;
	  }
	else if (event->character > 0)
	  {
	    ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	    ie.code = event->character;
	  }
	else
	  {
	    ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	    ie.code = event->keycode;
	  }

	ie.modifiers = emacs_mods;
	ie.timestamp = 0;
	kbd_buffer_store_event (&ie);
	return 1;
      }

    case WEB_EVT_MOUSE_DOWN:
    case WEB_EVT_MOUSE_UP:
      {
	struct input_event ie;
	EVENT_INIT (ie);
	ie.kind = MOUSE_CLICK_EVENT;
	ie.frame_or_window = frame;
	ie.code = event->button;
	XSETINT (ie.x, event->x);
	XSETINT (ie.y, event->y);
	ie.timestamp = 0;
	ie.modifiers = event->type == WEB_EVT_MOUSE_DOWN
	  ? down_modifier : up_modifier;
	if (event->mods & WIRE_MOD_SHIFT) ie.modifiers |= shift_modifier;
	if (event->mods & WIRE_MOD_CTRL)  ie.modifiers |= ctrl_modifier;
	if (event->mods & WIRE_MOD_META)  ie.modifiers |= meta_modifier;
	if (event->mods & WIRE_MOD_SUPER) ie.modifiers |= super_modifier;
	kbd_buffer_store_event (&ie);
	return 1;
      }

    case WEB_EVT_MOUSE_MOVE:
      dpyinfo->last_mouse_motion_x = event->x;
      dpyinfo->last_mouse_motion_y = event->y;
      dpyinfo->last_mouse_motion_frame = f;
      return 0;

    case WEB_EVT_SCROLL:
      {
	struct input_event ie;
	EVENT_INIT (ie);
	ie.kind = WHEEL_EVENT;
	ie.frame_or_window = frame;
	ie.code = 0;
	XSETINT (ie.x, event->x);
	XSETINT (ie.y, event->y);
	ie.timestamp = 0;
	ie.modifiers = 0;
	if (event->mods & WIRE_MOD_SHIFT) ie.modifiers |= shift_modifier;
	if (event->mods & WIRE_MOD_CTRL)  ie.modifiers |= ctrl_modifier;
	if (event->mods & WIRE_MOD_META)  ie.modifiers |= meta_modifier;
	if (event->dy > 0)
	  ie.modifiers |= down_modifier;
	else
	  ie.modifiers |= up_modifier;
	ie.arg = make_fixnum (abs (event->dy));
	kbd_buffer_store_event (&ie);
	return 1;
      }

    case WEB_EVT_RESIZE:
      if (event->cols > 0 && event->rows > 0)
	{
	  int cw = dpyinfo->default_char_width;
	  int ch = dpyinfo->default_char_height;
	  if (cw > 0 && ch > 0)
	    {
	      int non_text_w = FRAME_PIXEL_WIDTH (f) - FRAME_TEXT_WIDTH (f);
	      int non_text_h = FRAME_PIXEL_HEIGHT (f) - FRAME_TEXT_HEIGHT (f);
	      int new_pw = event->cols * cw + non_text_w;
	      int new_ph = event->rows * ch + non_text_h;
	      change_frame_size (f, new_pw, new_ph, false, true, false);
	      SET_FRAME_GARBAGED (f);
	      windows_or_buffers_changed = 63;
	      clear_current_matrices (f);
	    }
	}
      return 0;

    case WEB_EVT_FOCUS:
      {
	struct input_event ie;
	EVENT_INIT (ie);
	ie.frame_or_window = frame;
	ie.timestamp = 0;
	if (event->gained)
	  {
	    dpyinfo->x_focus_frame = f;
	    dpyinfo->x_focus_event_frame = f;
	    ie.kind = FOCUS_IN_EVENT;
	  }
	else
	  {
	    if (dpyinfo->x_focus_frame == f)
	      dpyinfo->x_focus_frame = NULL;
	    dpyinfo->x_focus_event_frame = NULL;
	    ie.kind = FOCUS_OUT_EVENT;
	  }
	kbd_buffer_store_event (&ie);
	return 1;
      }

    case WEB_EVT_FONT_METRICS:
      if (event->char_w > 0 && event->char_h > 0)
	{
	  dpyinfo->default_char_width = event->char_w;
	  dpyinfo->default_char_height = event->char_h;

	  FRAME_COLUMN_WIDTH (f) = event->char_w;
	  FRAME_LINE_HEIGHT (f) = event->char_h;

	  struct font *font = FRAME_FONT (f);
	  if (font)
	    {
	      font->ascent = event->asc > 0
		? event->asc : event->char_h * 3 / 4;
	      font->descent = event->desc > 0
		? event->desc : event->char_h - font->ascent;
	      font->height = event->char_h;
	      font->space_width = event->char_w;
	      font->average_width = event->char_w;
	      font->min_width = event->char_w;
	      font->max_width = event->char_w;
	    }

	  int non_text_w = FRAME_PIXEL_WIDTH (f) - FRAME_TEXT_WIDTH (f);
	  int non_text_h = FRAME_PIXEL_HEIGHT (f) - FRAME_TEXT_HEIGHT (f);
	  int new_pw = FRAME_COLS (f) * event->char_w + non_text_w;
	  int new_ph = FRAME_LINES (f) * event->char_h + non_text_h;
	  change_frame_size (f, new_pw, new_ph, false, true, false);
	  SET_FRAME_GARBAGED (f);
	  windows_or_buffers_changed = 63;
	}
      return 0;

    case WEB_EVT_INTERRUPT:
      kill (getpid (), SIGINT);
      return 0;

    case WEB_EVT_CLIPBOARD:
      if (strcmp (event->clipboard_dir, "paste") == 0
	  && event->clipboard_text[0])
	Vweb_clipboard = make_string_from_utf8
	  (event->clipboard_text, strlen (event->clipboard_text));
      return 0;

    case WEB_EVT_REQUEST_REDRAW:
      SET_FRAME_GARBAGED (f);
      windows_or_buffers_changed = 63;
      clear_current_matrices (f);
      return 0;

    case WEB_EVT_MENU_SELECT:
    case WEB_EVT_MENU_CANCEL:
      return 0;
    }

  return 0;
}
#endif /* HAVE_PTHREAD */

static int
web_read_socket (struct terminal *terminal,
		 struct input_event *hold_quit)
{
  struct web_display_info *dpyinfo = terminal->display_info.web;
  int count = 0;

#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      char drain[64];
      while (read (dpyinfo->async.notify_pipe[0], drain, sizeof drain) > 0)
	;

      struct web_event *events
	= web_event_queue_drain (&dpyinfo->async.input_queue);
      for (struct web_event *event = events; event; )
	{
	  struct web_event *next = event->next;
	  count += web_dispatch_event (dpyinfo, event, hold_quit);
	  web_event_recycle (&dpyinfo->async.input_queue, event);
	  event = next;
	}

      return count;
    }
#endif

  if (dpyinfo->proxy_fd < 0)
    return 0;

  /* Non-blocking read from proxy.  */
  int space = dpyinfo->read_buf_size - dpyinfo->read_buf_len;
  if (space > 0)
    {
      ssize_t n = read (dpyinfo->proxy_fd,
			dpyinfo->read_buf + dpyinfo->read_buf_len,
			space);
      if (n > 0)
	{
	  dpyinfo->read_buf_len += n;
	}
      else if (n == 0)
	return 0;  /* proxy exited */
    }

  struct frame *f = web_any_frame (dpyinfo);
  if (!f)
    return 0;

  /* Ensure the frame is considered focused when we receive data
     from the browser.  */
  if (!dpyinfo->x_focus_frame)
    dpyinfo->x_focus_frame = f;

  Lisp_Object frame;
  XSETFRAME (frame, f);

  /* Process complete NDJSON lines from the read buffer.  */
  int pos = 0;
  while (pos < dpyinfo->read_buf_len)
    {
      /* Find newline.  */
      int nl = -1;
      for (int i = pos; i < dpyinfo->read_buf_len; i++)
	{
	  if (dpyinfo->read_buf[i] == '\n')
	    {
	      nl = i;
	      break;
	    }
	}
      if (nl < 0)
	break; /* incomplete line */

      const char *line = (const char *)(dpyinfo->read_buf + pos);
      int line_len = nl - pos;
      pos = nl + 1;

      if (line_len < 2)
	continue; /* skip empty lines */

      /* Dispatch on type.  */
      if (json_type_is (line, line_len, "key"))
	{
	  int keycode = json_extract_int (line, line_len, "keycode");
	  int mods_wire = json_extract_int (line, line_len, "mods");
	  int utf32 = json_extract_int (line, line_len, "char");
	  struct input_event ie;
	  EVENT_INIT (ie);
	  ie.frame_or_window = frame;

	  /* Convert wire modifiers to Emacs modifiers.  */
	  int emacs_mods = 0;
	  if (mods_wire & WIRE_MOD_SHIFT) emacs_mods |= shift_modifier;
	  if (mods_wire & WIRE_MOD_CTRL)  emacs_mods |= ctrl_modifier;
	  if (mods_wire & WIRE_MOD_META)  emacs_mods |= meta_modifier;
	  if (mods_wire & WIRE_MOD_SUPER) emacs_mods |= super_modifier;

	  if (keycode >= 0xFF00)
	    {
	      ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	      ie.code = keycode;
	    }
	  else if (utf32 > 0 && utf32 < 128)
	    {
	      ie.kind = ASCII_KEYSTROKE_EVENT;
	      ie.code = utf32;
	    }
	  else if (utf32 > 0)
	    {
	      ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	      ie.code = utf32;
	    }
	  else
	    {
	      ie.kind = NON_ASCII_KEYSTROKE_EVENT;
	      ie.code = keycode;
	    }

	  ie.modifiers = emacs_mods;
	  ie.timestamp = 0;
	  kbd_buffer_store_event (&ie);
	  count++;
	}
      else if (json_type_is (line, line_len, "mouse_down")
	       || json_type_is (line, line_len, "mouse_up"))
	{
	  bool pressed = json_type_is (line, line_len, "mouse_down");
	  int mx = json_extract_int (line, line_len, "x");
	  int my = json_extract_int (line, line_len, "y");
	  int button = json_extract_int (line, line_len, "button");
	  int mods_wire = json_extract_int (line, line_len, "mods");

	  struct input_event ie;
	  EVENT_INIT (ie);
	  ie.kind = MOUSE_CLICK_EVENT;
	  ie.frame_or_window = frame;
	  ie.code = button;
	  XSETINT (ie.x, mx);
	  XSETINT (ie.y, my);
	  ie.timestamp = 0;
	  ie.modifiers = pressed ? down_modifier : up_modifier;
	  if (mods_wire & WIRE_MOD_SHIFT) ie.modifiers |= shift_modifier;
	  if (mods_wire & WIRE_MOD_CTRL)  ie.modifiers |= ctrl_modifier;
	  if (mods_wire & WIRE_MOD_META)  ie.modifiers |= meta_modifier;
	  if (mods_wire & WIRE_MOD_SUPER) ie.modifiers |= super_modifier;

	  kbd_buffer_store_event (&ie);
	  count++;
	}
      else if (json_type_is (line, line_len, "mouse_move"))
	{
	  dpyinfo->last_mouse_motion_x
	    = json_extract_int (line, line_len, "x");
	  dpyinfo->last_mouse_motion_y
	    = json_extract_int (line, line_len, "y");
	  dpyinfo->last_mouse_motion_frame = f;
	}
      else if (json_type_is (line, line_len, "scroll"))
	{
	  int sx = json_extract_int (line, line_len, "x");
	  int sy = json_extract_int (line, line_len, "y");
	  int dy = json_extract_int (line, line_len, "dy");
	  int mods_wire = json_extract_int (line, line_len, "mods");

	  struct input_event ie;
	  EVENT_INIT (ie);
	  ie.kind = WHEEL_EVENT;
	  ie.frame_or_window = frame;
	  ie.code = 0;
	  XSETINT (ie.x, sx);
	  XSETINT (ie.y, sy);
	  ie.timestamp = 0;
	  ie.modifiers = 0;
	  if (mods_wire & WIRE_MOD_SHIFT) ie.modifiers |= shift_modifier;
	  if (mods_wire & WIRE_MOD_CTRL)  ie.modifiers |= ctrl_modifier;
	  if (mods_wire & WIRE_MOD_META)  ie.modifiers |= meta_modifier;
	  if (dy > 0)
	    ie.modifiers |= down_modifier;
	  else
	    ie.modifiers |= up_modifier;

	  ie.arg = make_fixnum (abs (dy));
	  kbd_buffer_store_event (&ie);
	  count++;
	}
      else if (json_type_is (line, line_len, "resize"))
	{
	  int cols = json_extract_int (line, line_len, "cols");
	  int rows = json_extract_int (line, line_len, "rows");

	  if (cols > 0 && rows > 0 && f)
	    {
	      int cw = dpyinfo->default_char_width;
	      int ch = dpyinfo->default_char_height;
	      if (cw > 0 && ch > 0)
		{
		  int non_text_w = FRAME_PIXEL_WIDTH (f)
		    - FRAME_TEXT_WIDTH (f);
		  int non_text_h = FRAME_PIXEL_HEIGHT (f)
		    - FRAME_TEXT_HEIGHT (f);
		  int new_pw = cols * cw + non_text_w;
		  int new_ph = rows * ch + non_text_h;
		  change_frame_size (f, new_pw, new_ph,
				    false, true, false);
		  SET_FRAME_GARBAGED (f);
		  windows_or_buffers_changed = 63;
		  clear_current_matrices (f);
		}
	    }
	}
      else if (json_type_is (line, line_len, "focus"))
	{
	  bool gained = json_extract_bool (line, line_len, "gained");
	  if (gained)
	    {
	      dpyinfo->x_focus_frame = f;
	      dpyinfo->x_focus_event_frame = f;
	      /* Send FOCUS_IN_EVENT so Emacs updates internal focus
		 tracking and renders an active cursor.  */
	      struct input_event ie;
	      EVENT_INIT (ie);
	      ie.kind = FOCUS_IN_EVENT;
	      ie.frame_or_window = frame;
	      ie.timestamp = 0;
	      kbd_buffer_store_event (&ie);
	      count++;
	    }
	  else
	    {
	      if (dpyinfo->x_focus_frame == f)
		dpyinfo->x_focus_frame = NULL;
	      dpyinfo->x_focus_event_frame = NULL;
	      struct input_event ie;
	      EVENT_INIT (ie);
	      ie.kind = FOCUS_OUT_EVENT;
	      ie.frame_or_window = frame;
	      ie.timestamp = 0;
	      kbd_buffer_store_event (&ie);
	      count++;
	    }
	}
      else if (json_type_is (line, line_len, "font_metrics"))
	{
	  int cw = json_extract_int (line, line_len, "char_w");
	  int ch = json_extract_int (line, line_len, "char_h");
	  int asc = json_extract_int (line, line_len, "asc");
	  int desc = json_extract_int (line, line_len, "desc");

	  if (cw > 0 && ch > 0)
	    {
	      dpyinfo->default_char_width = cw;
	      dpyinfo->default_char_height = ch;

	      if (f)
		{
		  FRAME_COLUMN_WIDTH (f) = cw;
		  FRAME_LINE_HEIGHT (f) = ch;

		  struct font *font = FRAME_FONT (f);
		  if (font)
		    {
		      font->ascent = asc > 0 ? asc : ch * 3 / 4;
		      font->descent = desc > 0 ? desc : ch - font->ascent;
		      font->height = ch;
		      font->space_width = cw;
		      font->average_width = cw;
		      font->min_width = cw;
		      font->max_width = cw;
		    }

		  int non_text_w = FRAME_PIXEL_WIDTH (f)
		    - FRAME_TEXT_WIDTH (f);
		  int non_text_h = FRAME_PIXEL_HEIGHT (f)
		    - FRAME_TEXT_HEIGHT (f);
		  int new_pw = FRAME_COLS (f) * cw + non_text_w;
		  int new_ph = FRAME_LINES (f) * ch + non_text_h;
		  change_frame_size (f, new_pw, new_ph,
				    false, true, false);
		  SET_FRAME_GARBAGED (f);
		  windows_or_buffers_changed = 63;
		}
	    }
	}
      else if (json_type_is (line, line_len, "interrupt"))
	{
	  kill (getpid (), SIGINT);
	}
      else if (json_type_is (line, line_len, "clipboard"))
	{
	  char text_buf[65536];
	  int tlen = json_extract_string (line, line_len, "text",
					  text_buf, sizeof text_buf);
	  if (tlen > 0)
	    {
	      char dir_buf[16];
	      json_extract_string (line, line_len, "dir",
				   dir_buf, sizeof dir_buf);
	      if (strcmp (dir_buf, "paste") == 0)
		Vweb_clipboard
		  = make_string_from_utf8 (text_buf, tlen);
	    }
	}
      else if (json_type_is (line, line_len, "request_redraw"))
	{
	  if (f)
	    {
	      SET_FRAME_GARBAGED (f);
	      windows_or_buffers_changed = 63;
	      clear_current_matrices (f);
	    }
	}
    }

  /* Move unconsumed data to front.  */
  if (pos > 0)
    {
      dpyinfo->read_buf_len -= pos;
      if (dpyinfo->read_buf_len > 0)
	memmove (dpyinfo->read_buf, dpyinfo->read_buf + pos,
		 dpyinfo->read_buf_len);
    }

  return count;
}

static void
web_frame_up_to_date (struct frame *f)
{
  /* No-op.  */
}

static void
web_mouse_position (struct frame **fp, int insist,
		    Lisp_Object *bar_window,
		    enum scroll_bar_part *part,
		    Lisp_Object *x, Lisp_Object *y,
		    Time *timestamp)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (*fp);
  if (dpyinfo->last_mouse_motion_frame)
    {
      *fp = dpyinfo->last_mouse_motion_frame;
      XSETINT (*x, dpyinfo->last_mouse_motion_x);
      XSETINT (*y, dpyinfo->last_mouse_motion_y);
      *bar_window = Qnil;
      *part = scroll_bar_above_handle;
      *timestamp = dpyinfo->last_mouse_movement_time;
    }
}

static void
web_frame_rehighlight_hook (struct frame *f)
{
  /* No-op.  */
}

static void
web_frame_raise_lower (struct frame *f, bool raise_flag)
{
  /* No-op.  */
}

static void
web_make_frame_visible_invisible (struct frame *f, bool visible)
{
  if (visible)
    {
      SET_FRAME_VISIBLE (f, true);
      SET_FRAME_ICONIFIED (f, false);
    }
  else
    {
      SET_FRAME_VISIBLE (f, false);
    }
}

static void
web_fullscreen_hook (struct frame *f)
{
  /* No-op.  */
}

static void
web_set_vertical_scroll_bar (struct window *w, int portion,
			     int whole, int position)
{
  /* No-op: scroll bars disabled.  */
}

static void
web_set_horizontal_scroll_bar (struct window *w, int portion,
			       int whole, int position)
{
  /* No-op.  */
}

static void
web_condemn_scroll_bars (struct frame *f)
{
  /* No-op.  */
}

static void
web_redeem_scroll_bar (struct window *w)
{
  /* No-op.  */
}

static void
web_judge_scroll_bars (struct frame *f)
{
  /* No-op.  */
}

static const char *
web_get_string_resource (void *instance, const char *name,
			 const char *class)
{
  return NULL;
}

static void
web_destroy_window (struct frame *f)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  dpyinfo->reference_count--;
  xfree (f->output_data.web);
  f->output_data.web = NULL;
}

static void
web_delete_terminal (struct terminal *terminal)
{
  struct web_display_info *dpyinfo = terminal->display_info.web;
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      delete_read_fd (dpyinfo->async.notify_pipe[0]);
      web_async_shutdown (&dpyinfo->async);
      dpyinfo->async_enabled = false;
    }
#endif
  if (dpyinfo->proxy_fd >= 0)
    {
      delete_read_fd (dpyinfo->proxy_fd);
      close (dpyinfo->proxy_fd);
      dpyinfo->proxy_fd = -1;
    }
  if (dpyinfo->proxy_pid > 0)
    {
      kill (dpyinfo->proxy_pid, SIGTERM);
      waitpid (dpyinfo->proxy_pid, NULL, 0);
      dpyinfo->proxy_pid = 0;
    }
}


/* ====================================================================
   Color handling.
   ==================================================================== */

/* Basic X11 named colors.  */
struct named_color
{
  const char *name;
  unsigned long pixel;
};

static const struct named_color basic_colors[] = {
  /* Primary & basic.  */
  { "black",   0x000000 },
  { "white",   0xFFFFFF },
  { "red",     0xFF0000 },
  { "green",   0x00FF00 },
  { "blue",    0x0000FF },
  { "cyan",    0x00FFFF },
  { "magenta", 0xFF00FF },
  { "yellow",  0xFFFF00 },
  /* Grays.  */
  { "gray",    0xBEBEBE },
  { "grey",    0xBEBEBE },
  { "dark gray",    0xA9A9A9 },
  { "dark grey",    0xA9A9A9 },
  { "light gray",   0xD3D3D3 },
  { "light grey",   0xD3D3D3 },
  { "DimGray",      0x696969 },
  { "DimGrey",      0x696969 },
  { "SlateGray",    0x708090 },
  { "SlateGrey",    0x708090 },
  { "LightSlateGray",0x778899 },
  { "LightSlateGrey",0x778899 },
  { "DarkSlateGray", 0x2F4F4F },
  { "DarkSlateGrey", 0x2F4F4F },
  /* Reds / pinks.  */
  { "dark red",     0x8B0000 },
  { "IndianRed",    0xCD5C5C },
  { "firebrick",    0xB22222 },
  { "tomato",       0xFF6347 },
  { "coral",        0xFF7F50 },
  { "salmon",       0xFA8072 },
  { "LightSalmon",  0xFFA07A },
  { "DarkSalmon",   0xE9967A },
  { "pink",         0xFFC0CB },
  { "LightPink",    0xFFB6C1 },
  { "HotPink",      0xFF69B4 },
  { "DeepPink",     0xFF1493 },
  { "PaleVioletRed", 0xDB7093 },
  { "MediumVioletRed",0xC71585 },
  /* Oranges / browns.  */
  { "orange",       0xFFA500 },
  { "DarkOrange",   0xFF8C00 },
  { "OrangeRed",    0xFF4500 },
  { "brown",        0xA52A2A },
  { "chocolate",    0xD2691E },
  { "SaddleBrown",  0x8B4513 },
  { "sienna",       0xA0522D },
  { "peru",         0xCD853F },
  { "tan",          0xD2B48C },
  { "RosyBrown",    0xBC8F8F },
  /* Yellows / golds.  */
  { "gold",         0xFFD700 },
  { "GoldenRod",    0xDAA520 },
  { "DarkGoldenrod", 0xB8860B },
  { "LightGoldenrod", 0xEEDD82 },
  { "LightGoldenrodYellow", 0xFAFAD2 },
  { "PaleGoldenrod", 0xEEE8AA },
  { "khaki",        0xF0E68C },
  { "DarkKhaki",    0xBDB76B },
  { "LemonChiffon", 0xFFFACD },
  /* Greens.  */
  { "dark green",   0x006400 },
  { "ForestGreen",  0x228B22 },
  { "LimeGreen",    0x32CD32 },
  { "lime",         0x00FF00 },
  { "LawnGreen",    0x7CFC00 },
  { "chartreuse",   0x7FFF00 },
  { "GreenYellow",  0xADFF2F },
  { "SpringGreen",  0x00FF7F },
  { "MediumSpringGreen", 0x00FA9A },
  { "PaleGreen",    0x98FB98 },
  { "LightGreen",   0x90EE90 },
  { "DarkOliveGreen", 0x556B2F },
  { "OliveDrab",    0x6B8E23 },
  { "olive",        0x808000 },
  { "SeaGreen",     0x2E8B57 },
  { "MediumSeaGreen", 0x3CB371 },
  { "DarkSeaGreen", 0x8FBC8F },
  { "MediumAquamarine", 0x66CDAA },
  { "aquamarine",   0x7FFFD4 },
  /* Cyans / teals.  */
  { "dark cyan",    0x008B8B },
  { "teal",         0x008080 },
  { "DarkTurquoise", 0x00CED1 },
  { "MediumTurquoise", 0x48D1CC },
  { "turquoise",    0x40E0D0 },
  { "PaleTurquoise", 0xAFEEEE },
  { "LightCyan",    0xE0FFFF },
  { "CadetBlue",    0x5F9EA0 },
  /* Blues.  */
  { "dark blue",    0x00008B },
  { "MediumBlue",   0x0000CD },
  { "MidnightBlue", 0x191970 },
  { "navy",         0x000080 },
  { "NavyBlue",     0x000080 },
  { "RoyalBlue",    0x4169E1 },
  { "CornflowerBlue", 0x6495ED },
  { "DodgerBlue",   0x1E90FF },
  { "DeepSkyBlue",  0x00BFFF },
  { "SkyBlue",      0x87CEEB },
  { "LightSkyBlue", 0x87CEFA },
  { "LightBlue",    0xADD8E6 },
  { "SteelBlue",    0x4682B4 },
  { "LightSteelBlue", 0xB0C4DE },
  { "PowderBlue",   0xB0E0E6 },
  { "SlateBlue",    0x6A5ACD },
  { "DarkSlateBlue", 0x483D8B },
  { "MediumSlateBlue", 0x7B68EE },
  /* Purples / violets.  */
  { "dark magenta", 0x8B008B },
  { "purple",       0x800080 },
  { "violet",       0xEE82EE },
  { "DarkViolet",   0x9400D3 },
  { "BlueViolet",   0x8A2BE2 },
  { "MediumPurple", 0x9370DB },
  { "orchid",       0xDA70D6 },
  { "DarkOrchid",   0x9932CC },
  { "MediumOrchid", 0xBA55D3 },
  { "plum",         0xDDA0DD },
  { "thistle",      0xD8BFD8 },
  { "lavender",     0xE6E6FA },
  { "indigo",       0x4B0082 },
  /* Whites / pastels.  */
  { "snow",         0xFFFAFA },
  { "ivory",        0xFFFFF0 },
  { "honeydew",     0xF0FFF0 },
  { "MintCream",    0xF5FFFA },
  { "azure",        0xF0FFFF },
  { "AliceBlue",    0xF0F8FF },
  { "GhostWhite",   0xF8F8FF },
  { "FloralWhite",  0xFFFAF0 },
  { "seashell",     0xFFF5EE },
  { "beige",        0xF5F5DC },
  { "OldLace",      0xFDF5E6 },
  { "linen",        0xFAF0E6 },
  { "AntiqueWhite", 0xFAEBD7 },
  { "PapayaWhip",   0xFFEFD5 },
  { "BlanchedAlmond", 0xFFEBCD },
  { "bisque",       0xFFE4C4 },
  { "PeachPuff",    0xFFDAB9 },
  { "NavajoWhite",  0xFFDEAD },
  { "moccasin",     0xFFE4B5 },
  { "cornsilk",     0xFFF8DC },
  { "wheat",        0xF5DEB3 },
  { "LavenderBlush", 0xFFF0F5 },
  { "MistyRose",    0xFFE4E1 },
  /* X11 numbered color variants used by Emacs faces.  */
  { "blue1",    0x0000FF },
  { "blue2",    0x0000EE },
  { "blue3",    0x0000CD },
  { "blue4",    0x00008B },
  { "cyan1",    0x00FFFF },
  { "cyan2",    0x00EEEE },
  { "cyan3",    0x00CDCD },
  { "cyan4",    0x008B8B },
  { "green1",   0x00FF00 },
  { "green2",   0x00EE00 },
  { "green3",   0x00CD00 },
  { "green4",   0x008B00 },
  { "red1",     0xFF0000 },
  { "red2",     0xEE0000 },
  { "red3",     0xCD0000 },
  { "red4",     0x8B0000 },
  { "magenta1", 0xFF00FF },
  { "magenta2", 0xEE00EE },
  { "magenta3", 0xCD00CD },
  { "magenta4", 0x8B008B },
  { "yellow1",  0xFFFF00 },
  { "yellow2",  0xEEEE00 },
  { "yellow3",  0xCDCD00 },
  { "yellow4",  0x8B8B00 },
  { "orange1",  0xFF7F00 },
  { "orange2",  0xEE7600 },
  { "orange3",  0xCD6600 },
  { "orange4",  0x8B4500 },
  { "chocolate1", 0xFF7F24 },
  { "chocolate2", 0xEE7621 },
  { "chocolate3", 0xCD661D },
  { "brown1",   0xFF4040 },
  { "brown2",   0xEE3B3B },
  { "brown3",   0xCD3333 },
  { "brown4",   0x8B2323 },
  { "PaleVioletRed1", 0xFF82AB },
  { "PaleVioletRed2", 0xEE799F },
  { "PaleVioletRed3", 0xCD6889 },
  { "PaleTurquoise1", 0xBBFFFF },
  { "PaleTurquoise2", 0xAEEEEE },
  { "PaleTurquoise3", 0x96CDCD },
  { "PaleTurquoise4", 0x668B8B },
  { "DarkOliveGreen1", 0xCAFF70 },
  { "DarkOliveGreen2", 0xBCEE68 },
  { "DarkOliveGreen3", 0xA2CD5A },
  { "DarkSeaGreen1", 0xC1FFC1 },
  { "DarkSeaGreen2", 0xB4EEB4 },
  { NULL, 0 }
};

static bool
web_defined_color (struct frame *f, const char *name,
		   Emacs_Color *color_def, bool alloc_p,
		   bool make_index)
{
  unsigned short r16, g16, b16;

  /* Try parse_color_spec first (#RGB, rgb:, etc.).  */
  if (parse_color_spec (name, &r16, &g16, &b16))
    {
      color_def->red = r16;
      color_def->green = g16;
      color_def->blue = b16;
      color_def->pixel = ((r16 >> 8) << 16) | ((g16 >> 8) << 8) | (b16 >> 8);
      return true;
    }

  /* Try built-in color table.  */
  for (int i = 0; basic_colors[i].name; i++)
    {
      if (xstrcasecmp (name, basic_colors[i].name) == 0)
	{
	  unsigned long px = basic_colors[i].pixel;
	  color_def->pixel = px;
	  color_def->red = ((px >> 16) & 0xFF) * 257;
	  color_def->green = ((px >> 8) & 0xFF) * 257;
	  color_def->blue = (px & 0xFF) * 257;
	  return true;
	}
    }

  /* Handle "greyNN" / "grayNN" (X11 convention: NN = 0..100%).  */
  if ((strncasecmp (name, "grey", 4) == 0
       || strncasecmp (name, "gray", 4) == 0)
      && name[4] >= '0' && name[4] <= '9')
    {
      int pct = atoi (name + 4);
      if (pct >= 0 && pct <= 100)
	{
	  unsigned int v = (pct * 255 + 50) / 100;
	  color_def->pixel = (v << 16) | (v << 8) | v;
	  color_def->red = v * 257;
	  color_def->green = v * 257;
	  color_def->blue = v * 257;
	  return true;
	}
    }

  /* Handle "unspecified-fg" / "unspecified-bg".  */
  if (strcmp (name, "unspecified-fg") == 0)
    {
      color_def->pixel = f ? FRAME_FOREGROUND_COLOR (f) : 0xFFFFFF;
      color_def->red = ((color_def->pixel >> 16) & 0xFF) * 257;
      color_def->green = ((color_def->pixel >> 8) & 0xFF) * 257;
      color_def->blue = (color_def->pixel & 0xFF) * 257;
      return true;
    }
  if (strcmp (name, "unspecified-bg") == 0)
    {
      color_def->pixel = f ? FRAME_BACKGROUND_COLOR (f) : 0x000000;
      color_def->red = ((color_def->pixel >> 16) & 0xFF) * 257;
      color_def->green = ((color_def->pixel >> 8) & 0xFF) * 257;
      color_def->blue = (color_def->pixel & 0xFF) * 257;
      return true;
    }

  /* Unknown color name.  */
  fprintf (stderr, "web_defined_color: unknown color '%s'\n", name);
  color_def->pixel = 0xFF00FF;  /* magenta — conspicuous fallback */
  color_def->red = 0xFF * 257;
  color_def->green = 0;
  color_def->blue = 0xFF * 257;
  return true;
}

static Lisp_Object
web_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct font *font = XFONT_OBJECT (font_object);

  FRAME_FONT (f) = font;
  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = dpyinfo->default_char_width;
  FRAME_LINE_HEIGHT (f) = dpyinfo->default_char_height;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;

  /* Store the font back into the frame parameter.  */
  store_frame_param (f, Qfont,
		     Ffont_xlfd_name (font_object, Qnil, Qnil));

  return font_object;
}

static void
web_set_window_size (struct frame *f, bool change_gravity,
		     int width, int height)
{
  /* Send frame_size JSON to proxy.  */
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  char fg_hex[8], bg_hex[8];
  struct face *def = FACE_FROM_ID_OR_NULL (f, DEFAULT_FACE_ID);
  color_to_hex (def ? def->foreground : FRAME_FOREGROUND_COLOR (f), fg_hex);
  color_to_hex (def ? def->background : FRAME_BACKGROUND_COLOR (f), bg_hex);

  web_write_printf (dpyinfo,
		    "{\"type\":\"frame_size\",\"cols\":%d,\"rows\":%d,"
		    "\"default_face\":{\"fg\":\"%s\",\"bg\":\"%s\"}}\n",
		    FRAME_COLS (f), FRAME_LINES (f), fg_hex, bg_hex);
  web_control_flush (dpyinfo);
}

static void
web_query_frame_background_color (struct frame *f, Emacs_Color *bgcolor)
{
  unsigned long px = FRAME_BACKGROUND_COLOR (f);
  bgcolor->pixel = px;
  bgcolor->red = ((px >> 16) & 0xFF) * 257;
  bgcolor->green = ((px >> 8) & 0xFF) * 257;
  bgcolor->blue = (px & 0xFF) * 257;
}

static void
web_free_pixmap (struct frame *f, Emacs_Pixmap pixmap)
{
  /* No-op.  */
}

static void
web_implicitly_set_name (struct frame *f, Lisp_Object arg,
			 Lisp_Object oldval)
{
  /* No-op.  */
}

static void
web_iconify_frame (struct frame *f)
{
  /* No-op.  */
}

static void
web_set_scroll_bar_default_width (struct frame *f)
{
  FRAME_CONFIG_SCROLL_BAR_WIDTH (f) = 0;
  FRAME_CONFIG_SCROLL_BAR_COLS (f) = 0;
}

static void
web_set_scroll_bar_default_height (struct frame *f)
{
  FRAME_CONFIG_SCROLL_BAR_HEIGHT (f) = 0;
  FRAME_CONFIG_SCROLL_BAR_LINES (f) = 0;
}

static void
web_query_colors (struct frame *f, Emacs_Color *colors, int ncolors)
{
  for (int i = 0; i < ncolors; i++)
    {
      unsigned long pixel = colors[i].pixel;
      colors[i].red = ((pixel >> 16) & 0xFF) * 257;
      colors[i].green = ((pixel >> 8) & 0xFF) * 257;
      colors[i].blue = (pixel & 0xFF) * 257;
    }
}

static Lisp_Object
web_get_focus_frame (struct frame *f)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  if (dpyinfo->x_focus_frame)
    {
      Lisp_Object frame;
      XSETFRAME (frame, dpyinfo->x_focus_frame);
      return frame;
    }
  return Qnil;
}

static void
web_focus_frame (struct frame *f, bool noactivate)
{
  /* No-op.  */
}

static void
web_set_offset (struct frame *f, int x, int y, int change_gravity)
{
  /* No-op.  */
}

static Lisp_Object
web_menu_item_value (int sel_idx, int menuflags)
{
  if (sel_idx < 0 || sel_idx >= menu_items_used)
    return Qnil;

  Lisp_Object entry = AREF (menu_items, sel_idx + MENU_ITEMS_ITEM_VALUE);
  if (menuflags & MENU_KEYMAPS)
    {
      int pane_start = 0;
      for (int j = 0; j < sel_idx; j++)
	if (EQ (AREF (menu_items, j), Qt))
	  pane_start = j;

      Lisp_Object prefix = AREF (menu_items,
				 pane_start + MENU_ITEMS_PANE_PREFIX);
      entry = Fcons (entry, Qnil);
      if (!NILP (prefix))
	entry = Fcons (prefix, entry);
    }

  return entry;
}

static Lisp_Object
web_menu_show (struct frame *f, int x, int y, int menuflags,
	       Lisp_Object title, const char **error_name)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  *error_name = NULL;

  if (menu_items_n_panes == 0)
    return Qnil;

  if (menu_items_used <= MENU_ITEMS_PANE_LENGTH || !VECTORP (menu_items))
    {
      *error_name = "Empty menu";
      return Qnil;
    }

  /* Inhibit GC while we hold pointers into menu_items strings.  */
  specpdl_ref count = inhibit_garbage_collection ();

  /* Build JSON menu and send to browser.  Format:
     {"type":"menu","x":N,"y":N,"title":"...","panes":[
       {"name":"Pane","items":[
         {"label":"Item","key":"C-x","enabled":true,"pane":0,"idx":3},
         ...
       ]}, ...
     ]}  */

  WR_LIT (dpyinfo, "{\"type\":\"menu\"");
  web_write_printf (dpyinfo, ",\"x\":%d,\"y\":%d", x, y);

  if (STRINGP (title))
    {
      WR_LIT (dpyinfo, ",\"title\":");
      web_write_json_string (dpyinfo, SSDATA (title), SBYTES (title));
    }

  WR_LIT (dpyinfo, ",\"panes\":[");

  int pane_idx = 0;
  bool first_pane = true;
  int item_count = 0;
  int i = 0;

  while (i < menu_items_used)
    {
      if (EQ (AREF (menu_items, i), Qt))
	{
	  /* New pane.  */
	  if (!first_pane)
	    WR_LIT (dpyinfo, "]}");
	  first_pane = false;

	  Lisp_Object pane_name = AREF (menu_items, i + MENU_ITEMS_PANE_NAME);
	  const char *pname = STRINGP (pane_name) ? SSDATA (pane_name) : "";
	  ptrdiff_t plen = STRINGP (pane_name) ? SBYTES (pane_name) : 0;

	  if (pane_idx > 0)
	    WR_LIT (dpyinfo, ",");

	  WR_LIT (dpyinfo, "{\"name\":");
	  web_write_json_string (dpyinfo, pname, plen);
	  WR_LIT (dpyinfo, ",\"items\":[");

	  pane_idx++;
	  i += MENU_ITEMS_PANE_LENGTH;
	}
      else if (EQ (AREF (menu_items, i), Qquote))
	{
	  i += 1;
	}
      else if (NILP (AREF (menu_items, i)))
	{
	  i += 1;
	}
      else
	{
	  /* Menu item.  */
	  Lisp_Object item_name = AREF (menu_items, i + MENU_ITEMS_ITEM_NAME);
	  Lisp_Object enable = AREF (menu_items, i + MENU_ITEMS_ITEM_ENABLE);
	  Lisp_Object descrip = AREF (menu_items, i + MENU_ITEMS_ITEM_EQUIV_KEY);

	  if (STRINGP (item_name))
	    {
	      if (item_count > 0)
		WR_LIT (dpyinfo, ",");

	      WR_LIT (dpyinfo, "{\"label\":");
	      web_write_json_string (dpyinfo, SSDATA (item_name),
				     SBYTES (item_name));

	      if (STRINGP (descrip))
		{
		  WR_LIT (dpyinfo, ",\"key\":");
		  web_write_json_string (dpyinfo, SSDATA (descrip),
					SBYTES (descrip));
		}

	      web_write_printf (dpyinfo,
				",\"enabled\":%s,\"pane\":%d,\"idx\":%d",
				NILP (enable) ? "false" : "true",
				pane_idx - 1, i);

	      /* Check for separator (name is just dashes).  */
	      const char *nm = SSDATA (item_name);
	      if (nm[0] == '-' && (nm[1] == '-' || nm[1] == '\0'))
		WR_LIT (dpyinfo, ",\"separator\":true");

	      WR_LIT (dpyinfo, "}");
	      item_count++;
	    }

	  i += MENU_ITEMS_ITEM_LENGTH;
	}
    }

  if (!first_pane)
    WR_LIT (dpyinfo, "]}");
  WR_LIT (dpyinfo, "]}\n");
  web_control_flush (dpyinfo);

#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      Lisp_Object result = Qnil;
      bool got_response = false;

      while (!got_response)
	{
	  fd_set fds;
	  FD_ZERO (&fds);
	  FD_SET (dpyinfo->async.notify_pipe[0], &fds);
	  struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };

	  int ret = select (dpyinfo->async.notify_pipe[0] + 1,
			    &fds, NULL, NULL, &tv);
	  if (ret <= 0)
	    break;

	  char drain[64];
	  while (read (dpyinfo->async.notify_pipe[0],
		       drain, sizeof drain) > 0)
	    ;

	  struct web_event *events
	    = web_event_queue_drain (&dpyinfo->async.input_queue);
	  for (struct web_event *event = events; event; )
	    {
	      struct web_event *next = event->next;
	      if (event->type == WEB_EVT_MENU_SELECT)
		{
		  result = web_menu_item_value (event->menu_idx, menuflags);
		  got_response = true;
		}
	      else if (event->type == WEB_EVT_MENU_CANCEL)
		got_response = true;
	      else
		{
		  struct input_event hold_quit;
		  EVENT_INIT (hold_quit);
		  hold_quit.kind = NO_EVENT;
		  web_dispatch_event (dpyinfo, event, &hold_quit);
		  if (hold_quit.kind != NO_EVENT)
		    kbd_buffer_store_event (&hold_quit);
		}
	      web_event_recycle (&dpyinfo->async.input_queue, event);
	      event = next;
	    }
	}

      return unbind_to (count, result);
    }
#endif

  /* Block-wait for menu_select or menu_cancel from the browser.  */
  Lisp_Object result = Qnil;
  bool got_response = false;

  while (!got_response)
    {
      fd_set fds;
      FD_ZERO (&fds);
      FD_SET (dpyinfo->proxy_fd, &fds);
      struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };

      int ret = select (dpyinfo->proxy_fd + 1, &fds, NULL, NULL, &tv);
      if (ret <= 0)
	{
	  /* Timeout or error — cancel.  */
	  got_response = true;
	  break;
	}

      /* Read available data.  */
      int space = dpyinfo->read_buf_size - dpyinfo->read_buf_len;
      if (space > 0)
	{
	  ssize_t n = read (dpyinfo->proxy_fd,
			    dpyinfo->read_buf + dpyinfo->read_buf_len,
			    space);
	  if (n > 0)
	    dpyinfo->read_buf_len += n;
	  else if (n == 0)
	    break; /* proxy died */
	}

      /* Scan for complete lines.  */
      int pos = 0;
      while (pos < dpyinfo->read_buf_len)
	{
	  unsigned char *nl = memchr (dpyinfo->read_buf + pos,
				     '\n',
				     dpyinfo->read_buf_len - pos);
	  if (!nl)
	    break;

	  int line_len = nl - (dpyinfo->read_buf + pos);
	  char *line = (char *) dpyinfo->read_buf + pos;
	  pos = (nl - dpyinfo->read_buf) + 1;

	  if (json_type_is (line, line_len, "menu_select"))
	    {
	      int sel_idx = json_extract_int (line, line_len, "idx");
	      result = web_menu_item_value (sel_idx, menuflags);
	      got_response = true;
	    }
	  else if (json_type_is (line, line_len, "menu_cancel"))
	    {
	      got_response = true;
	    }
	  /* Ignore other messages while menu is open.  */
	}

      /* Move remaining data to front of buffer.  */
      if (pos > 0 && pos < dpyinfo->read_buf_len)
	{
	  memmove (dpyinfo->read_buf,
		   dpyinfo->read_buf + pos,
		   dpyinfo->read_buf_len - pos);
	  dpyinfo->read_buf_len -= pos;
	}
      else if (pos >= dpyinfo->read_buf_len)
	dpyinfo->read_buf_len = 0;
    }

  return unbind_to (count, result);
}

static void
web_activate_menubar (struct frame *f)
{
  /* No-op.  */
}

static Lisp_Object
web_popup_dialog (struct frame *f, Lisp_Object header,
		  Lisp_Object contents)
{
  /* No-op.  */
  return Qnil;
}


/* ====================================================================
   Terminal creation.
   ==================================================================== */

struct terminal *
web_create_terminal (struct web_display_info *dpyinfo)
{
  struct terminal *terminal;

  terminal = create_terminal (output_web, &web_redisplay_interface);

  terminal->display_info.web = dpyinfo;
  dpyinfo->terminal = terminal;

  terminal->clear_frame_hook = web_clear_frame;
  terminal->ring_bell_hook = web_ring_bell;
  terminal->toggle_invisible_pointer_hook = web_toggle_invisible_pointer;
  terminal->update_begin_hook = web_update_begin;
  terminal->update_end_hook = web_update_end;
  terminal->read_socket_hook = web_read_socket;
  terminal->frame_up_to_date_hook = web_frame_up_to_date;
  terminal->mouse_position_hook = web_mouse_position;
  terminal->frame_rehighlight_hook = web_frame_rehighlight_hook;
  terminal->frame_raise_lower_hook = web_frame_raise_lower;
  terminal->frame_visible_invisible_hook = web_make_frame_visible_invisible;
  terminal->fullscreen_hook = web_fullscreen_hook;
  terminal->set_vertical_scroll_bar_hook = web_set_vertical_scroll_bar;
  terminal->set_horizontal_scroll_bar_hook = web_set_horizontal_scroll_bar;
  terminal->condemn_scroll_bars_hook = web_condemn_scroll_bars;
  terminal->redeem_scroll_bar_hook = web_redeem_scroll_bar;
  terminal->judge_scroll_bars_hook = web_judge_scroll_bars;
  terminal->get_string_resource_hook = web_get_string_resource;
  terminal->delete_frame_hook = web_destroy_window;
  terminal->delete_terminal_hook = web_delete_terminal;
  terminal->query_frame_background_color = web_query_frame_background_color;
  terminal->defined_color_hook = web_defined_color;
  terminal->set_new_font_hook = web_new_font;
  terminal->implicit_set_name_hook = web_implicitly_set_name;
  terminal->iconify_frame_hook = web_iconify_frame;
  terminal->set_scroll_bar_default_width_hook
    = web_set_scroll_bar_default_width;
  terminal->set_scroll_bar_default_height_hook
    = web_set_scroll_bar_default_height;
  terminal->set_window_size_hook = web_set_window_size;
  terminal->query_colors = web_query_colors;
  terminal->get_focus_frame = web_get_focus_frame;
  terminal->focus_frame_hook = web_focus_frame;
  terminal->set_frame_offset_hook = web_set_offset;
  terminal->free_pixmap = web_free_pixmap;
  terminal->menu_show_hook = web_menu_show;
#ifdef HAVE_EXT_MENU_BAR
  terminal->activate_menubar_hook = web_activate_menubar;
#endif
  terminal->popup_dialog_hook = web_popup_dialog;

  return terminal;
}


/* ====================================================================
   Initialization — fork proxy, set up event loop.
   ==================================================================== */

/* Callback invoked by wait_reading_process_output when data arrives
   on the proxy fd.  */
#ifdef HAVE_PTHREAD
static void
web_notify_callback (int fd, void *data)
{
  struct web_display_info *dpyinfo = data;
  if (!dpyinfo || !dpyinfo->async_enabled
      || dpyinfo->async.notify_pipe[0] != fd)
    return;

  struct input_event hold_quit;
  EVENT_INIT (hold_quit);
  hold_quit.kind = NO_EVENT;

  int count = web_read_socket (dpyinfo->terminal, &hold_quit);
  (void) count;

  if (hold_quit.kind != NO_EVENT)
    kbd_buffer_store_event (&hold_quit);
}
#else
static void
web_proxy_fd_callback (int fd, void *data)
{
  struct web_display_info *dpyinfo = data;
  if (!dpyinfo || dpyinfo->proxy_fd != fd)
    return;

  struct input_event hold_quit;
  EVENT_INIT (hold_quit);
  hold_quit.kind = NO_EVENT;

  web_read_socket (dpyinfo->terminal, &hold_quit);

  if (hold_quit.kind != NO_EVENT)
    kbd_buffer_store_event (&hold_quit);
}
#endif

/* Set to true once the first web frame is fully created.  */
bool web_frame_ready;

/* Heartbeat timer callback — sends periodic heartbeat to browser.
   Does NOT force redisplay; the normal command loop handles that.
   Input is read reactively via the fd callback or via gobble_input
   triggered by the SIGALRM pending_signals mechanism.  */
static void
web_heartbeat_timer_callback (struct atimer *timer)
{
  struct web_display_info *dpyinfo = web_display_info;

  if (!web_frame_ready || !dpyinfo)
    return;

#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      /* In async mode the I/O thread sends heartbeats independently.
	 The SIGALRM just ensures gobble_input runs (via
	 pending_signals) so queued events are processed during
	 long-running Lisp.  Do NOT call Fredisplay here — it
	 fights with the command loop's own redisplay and adds
	 100+ ms latency to the typing path.  */
      return;
    }
#endif

  if (dpyinfo->proxy_fd < 0)
    return;

  /* Send heartbeat so the browser knows Emacs is alive.  */
  struct timespec now = current_timespec ();
  uint64_t ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  web_write_printf (dpyinfo,
		    "{\"type\":\"heartbeat\",\"ts\":%llu}\n",
		    (unsigned long long)ms);
  web_write_flush (dpyinfo);
}

bool
web_async_active_p (void)
{
#ifdef HAVE_PTHREAD
  return web_display_info && web_display_info->async_enabled
    && web_display_info->async.io_running;
#else
  return false;
#endif
}

bool
web_yield_due_p (void)
{
#ifdef HAVE_PTHREAD
  if (!web_async_active_p ())
    return false;

  struct web_async_state *async = &web_display_info->async;
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC, &now);
  long elapsed_ms = (now.tv_sec - async->last_yield_time.tv_sec) * 1000
    + (now.tv_nsec - async->last_yield_time.tv_nsec) / 1000000;
  if (elapsed_ms < 16)
    return false;

  async->last_yield_time = now;
  return true;
#else
  return false;
#endif
}

void
web_process_pending_events (void)
{
  if (detect_input_pending_run_timers (true))
    swallow_events (true);
}

void
web_redisplay_and_publish (void)
{
  if (!redisplaying_p && NILP (Vinhibit_redisplay)
      && !interrupt_input_blocked)
    redisplay_preserve_echo_area (2);
}

void
web_term_init (void)
{
  struct web_display_info *dpyinfo;

  block_input ();

  dpyinfo = xzalloc (sizeof *dpyinfo);
  dpyinfo->proxy_fd = -1;
  dpyinfo->proxy_pid = 0;
  dpyinfo->port = 8080;
  dpyinfo->n_planes = 24;
  dpyinfo->default_char_width = 10;
  dpyinfo->default_char_height = 20;
  dpyinfo->smallest_char_width = 1;
  dpyinfo->smallest_font_height = 1;
  dpyinfo->resx = 72.0;
  dpyinfo->resy = 72.0;
  dpyinfo->color_p = 1;
  dpyinfo->color_map = Qnil;
  dpyinfo->name_list_element = Fcons (build_string ("web"), Qnil);

  /* Allocate I/O buffers.  */
  dpyinfo->write_buf_size = WRITE_BUF_INIT_SIZE;
  dpyinfo->write_buf = xmalloc (dpyinfo->write_buf_size);
  dpyinfo->write_buf_len = 0;

  dpyinfo->read_buf_size = 64 * 1024;
  dpyinfo->read_buf = xmalloc (dpyinfo->read_buf_size);
  dpyinfo->read_buf_len = 0;

  /* Initialize JSON state.  */
  json_state_reset (&dpyinfo->json_state);

  web_display_info = dpyinfo;
  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  /* Create terminal and set up keyboard.  */
  struct terminal *terminal = web_create_terminal (dpyinfo);

  terminal->kboard = allocate_kboard (Qweb);
  terminal->kboard->reference_count++;
  /* If we're the only display, make this the current keyboard.  */
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;

  terminal->name = xstrdup ("web-display");

  /* Initialize fringe bitmaps.  */
  gui_init_fringe (&web_redisplay_interface);

  /* Fork the display proxy process.  */
  int sv[2];
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      unblock_input ();
      error ("web_term_init: socketpair failed: %s", strerror (errno));
      return;
    }

  pid_t pid = fork ();
  if (pid < 0)
    {
      close (sv[0]);
      close (sv[1]);
      unblock_input ();
      error ("web_term_init: fork failed: %s", strerror (errno));
      return;
    }

  if (pid == 0)
    {
      /* Child: exec the display proxy.  */
      close (sv[0]);

      char fd_str[16];
      snprintf (fd_str, sizeof fd_str, "%d", sv[1]);

      char port_str[16];
      snprintf (port_str, sizeof port_str, "%d", dpyinfo->port);

      /* Try several locations for the proxy binary.  */
      execlp ("emacs-web-display", "emacs-web-display",
	      "--emacs-fd", fd_str, "--port", port_str, NULL);

      /* Try relative to the Emacs binary.  */
      execl ("../web-display/emacs-web-display", "emacs-web-display",
	     "--emacs-fd", fd_str, "--port", port_str, NULL);

      execl ("web-display/emacs-web-display", "emacs-web-display",
	     "--emacs-fd", fd_str, "--port", port_str, NULL);

      _exit (127);
    }

  /* Parent.  */
  close (sv[1]);
  dpyinfo->proxy_fd = sv[0];
  dpyinfo->proxy_pid = pid;

  /* Set proxy fd to non-blocking.  */
  fcntl (dpyinfo->proxy_fd, F_SETFL,
	 fcntl (dpyinfo->proxy_fd, F_GETFL) | O_NONBLOCK);

#ifdef HAVE_PTHREAD
  if (web_async_init (&dpyinfo->async, dpyinfo->proxy_fd) < 0)
    {
      close (dpyinfo->proxy_fd);
      dpyinfo->proxy_fd = -1;
      unblock_input ();
      error ("web_term_init: async event loop failed");
      return;
    }

  if (web_async_start (&dpyinfo->async) < 0)
    {
      web_async_shutdown (&dpyinfo->async);
      unblock_input ();
      error ("web_term_init: async event loop failed");
      return;
    }

  dpyinfo->async_enabled = true;
  dpyinfo->proxy_fd = -1;
  add_read_fd (dpyinfo->async.notify_pipe[0], web_notify_callback, dpyinfo);
#else
  /* Register proxy fd with Emacs's event loop.  */
  add_read_fd (dpyinfo->proxy_fd, web_proxy_fd_callback, dpyinfo);
#endif

  unblock_input ();
}


DEFUN ("web--start-redisplay-timer", Fweb__start_redisplay_timer,
       Sweb__start_redisplay_timer, 0, 0, 0,
       doc: /* Start the periodic heartbeat timer for the web backend.
Sends heartbeats to the browser so it knows Emacs is alive.
Should be called after init completes and a frame exists.  */)
  (void)
{
  if (!web_frame_ready)
    return Qnil;

  /* 5Hz heartbeat — the SIGALRM also triggers gobble_input via
     the pending_signals mechanism, so input is polled at this rate
     during long-running Lisp.  For normal use, the fd callback
     provides near-instant input reactivity.  */
  struct timespec interval = make_timespec (0, 200000000); /* 200ms */
  start_atimer (ATIMER_CONTINUOUS, interval,
		web_heartbeat_timer_callback, NULL);
  return Qt;
}

DEFUN ("web-set-clipboard", Fweb_set_clipboard,
       Sweb_set_clipboard, 1, 1, 0,
       doc: /* Set the browser clipboard to STRING.
Sends STRING to the web display proxy, which forwards it to connected
browsers for writing to the system clipboard.  */)
  (Lisp_Object string)
{
  CHECK_STRING (string);

  struct web_display_info *dpyinfo = web_display_info;
  if (!dpyinfo)
    return Qnil;
#ifdef HAVE_PTHREAD
  if (!dpyinfo->async_enabled && dpyinfo->proxy_fd < 0)
    return Qnil;
#else
  if (dpyinfo->proxy_fd < 0)
    return Qnil;
#endif

  /* Encode string as UTF-8.  */
  Lisp_Object encoded = ENCODE_UTF_8 (string);
  const char *utf8 = (const char *) SDATA (encoded);
  ptrdiff_t len = SBYTES (encoded);

  /* Send clipboard JSON.  */
  WR_LIT (dpyinfo, "{\"type\":\"clipboard\",\"dir\":\"copy\",\"text\":");
  web_write_json_string (dpyinfo, utf8, (int)len);
  WR_LIT (dpyinfo, "}\n");
  web_control_flush (dpyinfo);

  return Qt;
}


/* Register Lisp symbols for the web backend.  */

void
syms_of_webterm (void)
{
  DEFSYM (Qweb, "web");

  defsubr (&Sweb__start_redisplay_timer);
  defsubr (&Sweb_set_clipboard);

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = 1;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = 0;

  DEFVAR_LISP ("x-toolkit-scroll-bars", Vx_toolkit_scroll_bars,
     doc: /* SKIP: real doc in xterm.c.  */);
  Vx_toolkit_scroll_bars = Qt;

  DEFVAR_LISP ("web-clipboard-text", Vweb_clipboard,
     doc: /* Most recent clipboard text received from the web browser.
When the user pastes in the browser, this variable is updated.  */);
  Vweb_clipboard = Qnil;

  Fprovide (Qweb, Qnil);
}
