/* libvterm bindings for the web display backend of GNU Emacs.

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

/* This implements the `web-vterm-*' primitives that lisp/web-term.el
   drives: a VT-compatible terminal emulator whose screen is rendered
   into an ordinary Emacs buffer with anonymous-plist faces.  libvterm
   does all escape-sequence parsing; this layer only moves bytes in
   and converts the cell grid to buffer text.

   Threading: all primitives run on whatever Lisp thread calls them
   (normally the process-filter context).  Handles are plain ints into
   a static table; the table is only touched while the global Lisp
   lock is held, so no extra locking is needed.  */

#include <config.h>

#ifdef HAVE_VTERM

#include <string.h>
#include <stdio.h>

#include <vterm.h>

#include "lisp.h"
#include "buffer.h"
#include "character.h"
#include "coding.h"
#include "termhooks.h"
#include "webterm.h"

#define WEBVTERM_MAX_INSTANCES 32
#define WEBVTERM_TITLE_MAX 256

struct webvterm
{
  VTerm *vt;
  VTermScreen *vts;
  int rows, cols;
  bool cursor_visible;
  bool title_set;
  size_t title_len;
  char title[WEBVTERM_TITLE_MAX];
  bool in_use;
};

static struct webvterm instances[WEBVTERM_MAX_INSTANCES];

static struct webvterm *
webvterm_get (Lisp_Object handle)
{
  CHECK_FIXNUM (handle);
  EMACS_INT h = XFIXNUM (handle);
  if (h < 0 || h >= WEBVTERM_MAX_INSTANCES || !instances[h].in_use)
    error ("Invalid web-vterm handle: %"pI"d", h);
  return &instances[h];
}

/* — Screen callbacks —

   We re-render the whole screen on every update, so damage tracking
   is unnecessary; we only listen for termprops (title, cursor
   visibility).  Cursor position is queried from the state layer on
   demand.  */

static int
webvterm_settermprop (VTermProp prop, VTermValue *val, void *user)
{
  struct webvterm *wv = user;
  switch (prop)
    {
    case VTERM_PROP_CURSORVISIBLE:
      wv->cursor_visible = val->boolean;
      return 1;
    case VTERM_PROP_TITLE:
      {
	VTermStringFragment frag = val->string;
	if (frag.initial)
	  wv->title_len = 0;
	size_t room = WEBVTERM_TITLE_MAX - wv->title_len;
	size_t n = frag.len < room ? frag.len : room;
	memcpy (wv->title + wv->title_len, frag.str, n);
	wv->title_len += n;
	if (frag.final)
	  wv->title_set = true;
	return 1;
      }
    default:
      return 0;
    }
}

static VTermScreenCallbacks webvterm_screen_callbacks = {
  .settermprop = webvterm_settermprop,
};

/* — Rendering —  */

/* Build the face plist for a cell run, or nil for plain text.  */

static Lisp_Object
webvterm_cell_face (struct webvterm *wv, const VTermScreenCell *cell)
{
  Lisp_Object face = Qnil;
  char colbuf[8];

  /* Build in reverse so the plist reads naturally.  */
  if (cell->attrs.strike)
    face = Fcons (QCstrike_through, Fcons (Qt, face));
  if (cell->attrs.underline != VTERM_UNDERLINE_OFF)
    face = Fcons (QCunderline, Fcons (Qt, face));
  if (cell->attrs.reverse)
    face = Fcons (QCinverse_video, Fcons (Qt, face));
  if (cell->attrs.italic)
    face = Fcons (QCslant, Fcons (Qitalic, face));
  if (cell->attrs.bold)
    face = Fcons (QCweight, Fcons (Qbold, face));

  VTermColor fg = cell->fg, bg = cell->bg;
  if (!VTERM_COLOR_IS_DEFAULT_BG (&bg))
    {
      vterm_screen_convert_color_to_rgb (wv->vts, &bg);
      sprintf (colbuf, "#%02x%02x%02x", bg.rgb.red, bg.rgb.green,
	       bg.rgb.blue);
      face = Fcons (QCbackground, Fcons (build_string (colbuf), face));
    }
  if (!VTERM_COLOR_IS_DEFAULT_FG (&fg))
    {
      vterm_screen_convert_color_to_rgb (wv->vts, &fg);
      sprintf (colbuf, "#%02x%02x%02x", fg.rgb.red, fg.rgb.green,
	       fg.rgb.blue);
      face = Fcons (QCforeground, Fcons (build_string (colbuf), face));
    }
  return face;
}

/* True if A and B render with identical face attributes.  */

static bool
webvterm_same_attrs (const VTermScreenCell *a, const VTermScreenCell *b)
{
  return (a->attrs.bold == b->attrs.bold
	  && a->attrs.underline == b->attrs.underline
	  && a->attrs.italic == b->attrs.italic
	  && a->attrs.reverse == b->attrs.reverse
	  && a->attrs.strike == b->attrs.strike
	  && vterm_color_is_equal (&a->fg, &b->fg)
	  && vterm_color_is_equal (&a->bg, &b->bg));
}

/* Flush BUF (NBYTES of UTF-8, NCHARS characters) into the current
   buffer with FACE.  */

static void
webvterm_flush_run (char *buf, ptrdiff_t nbytes, ptrdiff_t nchars,
		    Lisp_Object face)
{
  if (nbytes == 0)
    return;
  Lisp_Object s = make_multibyte_string (buf, nchars, nbytes);
  if (!NILP (face))
    Fput_text_property (make_fixnum (0), make_fixnum (nchars),
			Qface, face, s);
  Finsert (1, &s);
}

/* Erase BUFFER and render the entire vterm screen into it.  */

static void
webvterm_render (struct webvterm *wv, Lisp_Object buffer)
{
  CHECK_BUFFER (buffer);
  if (!BUFFER_LIVE_P (XBUFFER (buffer)))
    error ("Attempt to render web-vterm into a dead buffer");

  specpdl_ref count = SPECPDL_INDEX ();
  record_unwind_current_buffer ();
  Fset_buffer (buffer);
  Ferase_buffer ();

  /* Worst case: every cell is a full 6-codepoint grapheme.  */
  ptrdiff_t runbuf_size = wv->cols * VTERM_MAX_CHARS_PER_CELL * MAX_MULTIBYTE_LENGTH;
  USE_SAFE_ALLOCA;
  char *runbuf = SAFE_ALLOCA (runbuf_size);

  for (int row = 0; row < wv->rows; row++)
    {
      if (row > 0)
	{
	  Lisp_Object nl = build_string ("\n");
	  Finsert (1, &nl);
	}

      VTermScreenCell runcell;
      bool have_run = false;
      ptrdiff_t nbytes = 0, nchars = 0;

      for (int col = 0; col < wv->cols; )
	{
	  VTermScreenCell cell;
	  VTermPos pos = { .row = row, .col = col };
	  if (!vterm_screen_get_cell (wv->vts, pos, &cell))
	    memset (&cell, 0, sizeof cell);

	  if (have_run && !webvterm_same_attrs (&runcell, &cell))
	    {
	      webvterm_flush_run (runbuf, nbytes, nchars,
				  webvterm_cell_face (wv, &runcell));
	      nbytes = nchars = 0;
	      have_run = false;
	    }
	  if (!have_run)
	    {
	      runcell = cell;
	      have_run = true;
	    }

	  if (cell.chars[0] == 0)
	    {
	      runbuf[nbytes++] = ' ';
	      nchars++;
	    }
	  else
	    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
	      {
		int c = cell.chars[i];
		if (!CHAR_VALID_P (c))
		  c = 0xFFFD;
		nbytes += CHAR_STRING (c, (unsigned char *) runbuf + nbytes);
		nchars++;
	      }

	  col += cell.width > 0 ? cell.width : 1;
	}

      if (have_run)
	webvterm_flush_run (runbuf, nbytes, nchars,
			    webvterm_cell_face (wv, &runcell));
    }

  SAFE_FREE ();
  unbind_to (count, Qnil);
}

/* Drain libvterm's pending output buffer into a unibyte string.  */

static Lisp_Object
webvterm_drain_output (struct webvterm *wv)
{
  char buf[4096];
  Lisp_Object acc = Qnil;
  size_t n;
  while ((n = vterm_output_read (wv->vt, buf, sizeof buf)) > 0)
    {
      Lisp_Object chunk = make_unibyte_string (buf, n);
      acc = NILP (acc) ? chunk : concat2 (acc, chunk);
      if (n < sizeof buf)
	break;
    }
  return NILP (acc) ? empty_unibyte_string : acc;
}

/* — Primitives —  */

DEFUN ("web-vterm-new", Fweb_vterm_new, Sweb_vterm_new, 2, 2, 0,
       doc: /* Create a new libvterm instance of ROWS x COLS.
Returns an integer handle for use with the other web-vterm functions.  */)
  (Lisp_Object rows, Lisp_Object cols)
{
  CHECK_FIXNAT (rows);
  CHECK_FIXNAT (cols);
  int r = max (1, min (XFIXNAT (rows), 1000));
  int c = max (1, min (XFIXNAT (cols), 1000));

  int h;
  for (h = 0; h < WEBVTERM_MAX_INSTANCES; h++)
    if (!instances[h].in_use)
      break;
  if (h == WEBVTERM_MAX_INSTANCES)
    error ("Too many web-vterm instances (max %d)", WEBVTERM_MAX_INSTANCES);

  struct webvterm *wv = &instances[h];
  memset (wv, 0, sizeof *wv);
  wv->vt = vterm_new (r, c);
  if (!wv->vt)
    error ("Failed to create vterm instance");
  vterm_set_utf8 (wv->vt, 1);
  wv->vts = vterm_obtain_screen (wv->vt);
  vterm_screen_set_callbacks (wv->vts, &webvterm_screen_callbacks, wv);
  vterm_screen_set_damage_merge (wv->vts, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen (wv->vts, 1);
  vterm_screen_reset (wv->vts, 1);
  wv->rows = r;
  wv->cols = c;
  wv->cursor_visible = true;
  wv->in_use = true;
  return make_fixnum (h);
}

DEFUN ("web-vterm-destroy", Fweb_vterm_destroy, Sweb_vterm_destroy, 1, 1, 0,
       doc: /* Destroy the libvterm instance HANDLE.  */)
  (Lisp_Object handle)
{
  struct webvterm *wv = webvterm_get (handle);
  vterm_free (wv->vt);
  wv->vt = NULL;
  wv->vts = NULL;
  wv->in_use = false;
  return Qnil;
}

DEFUN ("web-vterm-write", Fweb_vterm_write, Sweb_vterm_write, 2, 2, 0,
       doc: /* Feed STRING (raw process output bytes) to vterm HANDLE's parser.  */)
  (Lisp_Object handle, Lisp_Object string)
{
  struct webvterm *wv = webvterm_get (handle);
  CHECK_STRING (string);
  /* Process filters with :coding no-conversion hand us unibyte
     strings; encode anything else as UTF-8, which is what the
     terminal speaks.  */
  if (STRING_MULTIBYTE (string))
    string = ENCODE_UTF_8 (string);
  vterm_input_write (wv->vt, SSDATA (string), SBYTES (string));
  vterm_screen_flush_damage (wv->vts);
  return Qnil;
}

DEFUN ("web-vterm-update", Fweb_vterm_update, Sweb_vterm_update, 2, 2, 0,
       doc: /* Render vterm HANDLE's screen contents into BUFFER.
The buffer is erased and refilled with propertized text.  */)
  (Lisp_Object handle, Lisp_Object buffer)
{
  struct webvterm *wv = webvterm_get (handle);
  webvterm_render (wv, buffer);
  return Qnil;
}

DEFUN ("web-vterm-init-buffer", Fweb_vterm_init_buffer,
       Sweb_vterm_init_buffer, 2, 2, 0,
       doc: /* Initialize BUFFER with vterm HANDLE's screen.
Currently identical to `web-vterm-update'.  */)
  (Lisp_Object handle, Lisp_Object buffer)
{
  struct webvterm *wv = webvterm_get (handle);
  webvterm_render (wv, buffer);
  return Qnil;
}

DEFUN ("web-vterm-get-cursor", Fweb_vterm_get_cursor,
       Sweb_vterm_get_cursor, 1, 1, 0,
       doc: /* Return vterm HANDLE's cursor position as (ROW . COL), 0-indexed.  */)
  (Lisp_Object handle)
{
  struct webvterm *wv = webvterm_get (handle);
  VTermPos pos;
  vterm_state_get_cursorpos (vterm_obtain_state (wv->vt), &pos);
  return Fcons (make_fixnum (pos.row), make_fixnum (pos.col));
}

DEFUN ("web-vterm-cursor-visible-p", Fweb_vterm_cursor_visible_p,
       Sweb_vterm_cursor_visible_p, 1, 1, 0,
       doc: /* Return t if vterm HANDLE's cursor is visible.  */)
  (Lisp_Object handle)
{
  return webvterm_get (handle)->cursor_visible ? Qt : Qnil;
}

DEFUN ("web-vterm-get-title", Fweb_vterm_get_title,
       Sweb_vterm_get_title, 1, 1, 0,
       doc: /* Return the window title set by the application in vterm HANDLE.
Returns nil if no title has been set.  */)
  (Lisp_Object handle)
{
  struct webvterm *wv = webvterm_get (handle);
  if (!wv->title_set || wv->title_len == 0)
    return Qnil;
  /* The title arrives as UTF-8 from the OSC sequence.  */
  return make_string_from_utf8 (wv->title, wv->title_len);
}

DEFUN ("web-vterm-get-output", Fweb_vterm_get_output,
       Sweb_vterm_get_output, 1, 1, 0,
       doc: /* Return pending output from vterm HANDLE for the PTY.
This is data the terminal wants to send to the child process
(query responses, mode acknowledgements).  Returns a unibyte string,
empty if there is nothing pending.  */)
  (Lisp_Object handle)
{
  return webvterm_drain_output (webvterm_get (handle));
}

DEFUN ("web-vterm-key-input", Fweb_vterm_key_input,
       Sweb_vterm_key_input, 3, 3, 0,
       doc: /* Encode a key press for vterm HANDLE and return the bytes to send.
KEY is a character or a symbol (return, tab, backspace, escape, up,
down, left, right, home, end, prior, next, delete, insert, f1..f12).
MODS is a bitmask: 1=shift, 2=alt/meta, 4=ctrl.
Returns a unibyte string to write to the PTY.  */)
  (Lisp_Object handle, Lisp_Object key, Lisp_Object mods)
{
  struct webvterm *wv = webvterm_get (handle);
  CHECK_FIXNUM (mods);
  /* The Lisp bitmask deliberately matches VTermModifier.  */
  VTermModifier mod = XFIXNUM (mods) & (VTERM_MOD_SHIFT | VTERM_MOD_ALT
					| VTERM_MOD_CTRL);

  if (FIXNUMP (key))
    vterm_keyboard_unichar (wv->vt, XFIXNUM (key), mod);
  else if (SYMBOLP (key))
    {
      const char *name = SSDATA (SYMBOL_NAME (key));
      VTermKey vk = VTERM_KEY_NONE;
      if (!strcmp (name, "return"))         vk = VTERM_KEY_ENTER;
      else if (!strcmp (name, "tab"))       vk = VTERM_KEY_TAB;
      else if (!strcmp (name, "backspace")) vk = VTERM_KEY_BACKSPACE;
      else if (!strcmp (name, "escape"))    vk = VTERM_KEY_ESCAPE;
      else if (!strcmp (name, "up"))        vk = VTERM_KEY_UP;
      else if (!strcmp (name, "down"))      vk = VTERM_KEY_DOWN;
      else if (!strcmp (name, "left"))      vk = VTERM_KEY_LEFT;
      else if (!strcmp (name, "right"))     vk = VTERM_KEY_RIGHT;
      else if (!strcmp (name, "home"))      vk = VTERM_KEY_HOME;
      else if (!strcmp (name, "end"))       vk = VTERM_KEY_END;
      else if (!strcmp (name, "prior"))     vk = VTERM_KEY_PAGEUP;
      else if (!strcmp (name, "next"))      vk = VTERM_KEY_PAGEDOWN;
      else if (!strcmp (name, "delete"))    vk = VTERM_KEY_DEL;
      else if (!strcmp (name, "insert"))    vk = VTERM_KEY_INS;
      else if (name[0] == 'f' && name[1] >= '0' && name[1] <= '9')
	{
	  int n = atoi (name + 1);
	  if (n >= 1 && n <= 63)
	    vk = VTERM_KEY_FUNCTION (n);
	}
      if (vk == VTERM_KEY_NONE)
	error ("Unknown web-vterm key symbol: %s", name);
      vterm_keyboard_key (wv->vt, vk, mod);
    }
  else
    wrong_type_argument (Qintegerp, key);

  return webvterm_drain_output (wv);
}

DEFUN ("web-vterm-resize", Fweb_vterm_resize, Sweb_vterm_resize, 3, 3, 0,
       doc: /* Resize vterm HANDLE to ROWS x COLS.  */)
  (Lisp_Object handle, Lisp_Object rows, Lisp_Object cols)
{
  struct webvterm *wv = webvterm_get (handle);
  CHECK_FIXNAT (rows);
  CHECK_FIXNAT (cols);
  int r = max (1, min (XFIXNAT (rows), 1000));
  int c = max (1, min (XFIXNAT (cols), 1000));
  if (r != wv->rows || c != wv->cols)
    {
      wv->rows = r;
      wv->cols = c;
      vterm_set_size (wv->vt, r, c);
      vterm_screen_flush_damage (wv->vts);
    }
  return Qnil;
}

void
syms_of_webvterm (void)
{
  defsubr (&Sweb_vterm_new);
  defsubr (&Sweb_vterm_destroy);
  defsubr (&Sweb_vterm_write);
  defsubr (&Sweb_vterm_update);
  defsubr (&Sweb_vterm_init_buffer);
  defsubr (&Sweb_vterm_get_cursor);
  defsubr (&Sweb_vterm_cursor_visible_p);
  defsubr (&Sweb_vterm_get_title);
  defsubr (&Sweb_vterm_get_output);
  defsubr (&Sweb_vterm_key_input);
  defsubr (&Sweb_vterm_resize);
}

#endif /* HAVE_VTERM */
