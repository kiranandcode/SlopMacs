/* Web font driver for GNU Emacs.

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

/* Simple font driver for the web display backend.  All actual text
   rendering happens in the browser via Canvas 2D, so this driver just
   provides basic metrics for Emacs layout calculations.  Metrics are
   initially approximated and then updated when the browser reports
   actual font measurements over the WebSocket connection.  */

#include <config.h>

#include "lisp.h"
#include "dispextern.h"
#include "frame.h"
#include "font.h"
#include "webterm.h"
#include "pdumper.h"

static Lisp_Object font_cache;

static Lisp_Object
webfont_get_cache (struct frame *frame)
{
  return font_cache;
}

/* Return a list of font entities matching FONT_SPEC on FRAME.  */

static Lisp_Object
webfont_list (struct frame *f, Lisp_Object font_spec)
{
  Lisp_Object entity = font_make_entity ();
  Lisp_Object family = AREF (font_spec, FONT_FAMILY_INDEX);

  ASET (entity, FONT_TYPE_INDEX, Qweb);
  ASET (entity, FONT_FOUNDRY_INDEX, Qweb);
  ASET (entity, FONT_FAMILY_INDEX,
	NILP (family) ? intern ("monospace") : family);
  ASET (entity, FONT_ADSTYLE_INDEX, Qnil);
  ASET (entity, FONT_REGISTRY_INDEX, intern ("iso10646-1"));
  ASET (entity, FONT_SIZE_INDEX, make_fixnum (0));
  ASET (entity, FONT_AVGWIDTH_INDEX, make_fixnum (0));
  ASET (entity, FONT_SPACING_INDEX,
	make_fixnum (FONT_SPACING_MONO));

  /* Copy weight/slant/width from spec if specified.
     These values use the encoded (numeric << 8 | index << 4) format.
     Leave unset values as Qnil so font matching treats them as "any".  */
  ASET (entity, FONT_WEIGHT_INDEX,
	AREF (font_spec, FONT_WEIGHT_INDEX));
  ASET (entity, FONT_SLANT_INDEX,
	AREF (font_spec, FONT_SLANT_INDEX));
  ASET (entity, FONT_WIDTH_INDEX,
	AREF (font_spec, FONT_WIDTH_INDEX));

  return list1 (entity);
}

/* Return the best match for FONT_SPEC on frame F.  */

static Lisp_Object
webfont_match (struct frame *f, Lisp_Object font_spec)
{
  return XCAR (webfont_list (f, font_spec));
}

/* Open a font entity ENTITY for frame F at pixel size PIXEL_SIZE.  */

static Lisp_Object
webfont_open (struct frame *f, Lisp_Object entity, int pixel_size)
{
  struct web_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  if (pixel_size == 0)
    pixel_size = 16;  /* Default size.  */

  Lisp_Object font_object
    = font_make_object (VECSIZE (struct font), entity, pixel_size);
  struct font *font = XFONT_OBJECT (font_object);

  font->driver = &webfont_driver;
  font->pixel_size = pixel_size;

  /* Use metrics from the browser if available, otherwise approximate.
     The browser sends actual font metrics over WebSocket on connect.  */
  int char_w = dpyinfo->default_char_width;
  int char_h = dpyinfo->default_char_height;

  if (char_w <= 0)
    char_w = pixel_size * 6 / 10;  /* ~0.6 em for monospace.  */
  if (char_h <= 0)
    char_h = pixel_size;

  font->ascent = char_h * 3 / 4;
  font->descent = char_h - font->ascent;
  font->height = char_h;
  font->space_width = char_w;
  font->average_width = char_w;
  font->min_width = char_w;
  font->max_width = char_w;
  font->underline_position = -1;
  font->underline_thickness = 1;
  font->baseline_offset = 0;

  ASET (font_object, FONT_TYPE_INDEX, Qweb);
  ASET (font_object, FONT_SIZE_INDEX, make_fixnum (pixel_size));

  /* Set font name — needed by gui_default_parameter.  */
  Lisp_Object xlfd_name = Ffont_xlfd_name (font_object, Qnil, Qt);
  if (NILP (xlfd_name))
    {
      Lisp_Object fam = AREF (entity, FONT_FAMILY_INDEX);
      char buf[256];
      snprintf (buf, sizeof buf, "%s-%d",
		SYMBOLP (fam) ? SSDATA (SYMBOL_NAME (fam)) : "monospace",
		pixel_size);
      xlfd_name = build_string (buf);
    }
  font->props[FONT_NAME_INDEX] = xlfd_name;
  font->props[FONT_FULLNAME_INDEX] = xlfd_name;

  return font_object;
}

static void
webfont_close (struct font *font)
{
  /* Nothing to free — rendering is done in the browser.  */
}

static void
webfont_prepare_face (struct frame *f, struct face *face)
{
  /* No special preparation needed.  */
}

static unsigned
webfont_encode_char (struct font *font, int c)
{
  /* Pass through Unicode codepoints directly to the browser.  */
  return (c >= 0 && c < 0x110000) ? (unsigned) c : FONT_INVALID_CODE;
}

static void
webfont_text_extents (struct font *font, const unsigned *code,
		      int nglyphs, struct font_metrics *metrics)
{
  memset (metrics, 0, sizeof (*metrics));
  metrics->width = font->average_width * nglyphs;
  metrics->ascent = font->ascent;
  metrics->descent = font->descent;
  metrics->lbearing = 0;
  metrics->rbearing = metrics->width;
}

static int
webfont_draw (struct glyph_string *s, int from, int to,
	      int x, int y, bool with_background)
{
  /* Drawing is handled by web_draw_glyph_string in webterm.c,
     which serializes the entire glyph string.  This callback is
     not used directly by the web backend.  */
  return 0;
}

struct font_driver const webfont_driver =
  {
    .type = LISPSYM_INITIALLY (Qweb),
    .case_sensitive = true,
    .get_cache = webfont_get_cache,
    .list = webfont_list,
    .match = webfont_match,
    .open_font = webfont_open,
    .close_font = webfont_close,
    .prepare_face = webfont_prepare_face,
    .encode_char = webfont_encode_char,
    .text_extents = webfont_text_extents,
    .draw = webfont_draw,
  };

static void
syms_of_webfont_for_pdumper (void)
{
  register_font_driver (&webfont_driver, NULL);
}

void
syms_of_webfont (void)
{
  pdumper_do_now_and_after_load (syms_of_webfont_for_pdumper);

  font_cache = list1 (Qnil);
  staticpro (&font_cache);
}
