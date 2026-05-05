/* Functions for the web display backend of GNU Emacs.

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

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "lisp.h"
#include "frame.h"
#include "window.h"
#include "termhooks.h"
#include "webterm.h"
#include "webgui.h"
#include "dispextern.h"
#include "font.h"
#include "coding.h"


/* Stub frame parameter handlers.  For most params we use generic
   gui_* functions; for params we don't support yet we use a stub
   that does nothing.  */

static void
web_set_foreground_color (struct frame *f, Lisp_Object arg,
			  Lisp_Object oldval)
{
  Emacs_Color col;

  CHECK_STRING (arg);
  if (!FRAME_TERMINAL (f)->defined_color_hook (f, SSDATA (arg),
					       &col, true, false))
    error ("Undefined color: %s", SSDATA (arg));

  FRAME_FOREGROUND_COLOR (f) = col.pixel;
  update_face_from_frame_parameter (f, Qforeground_color, arg);
  if (FRAME_VISIBLE_P (f))
    SET_FRAME_GARBAGED (f);
}

static void
web_set_background_color (struct frame *f, Lisp_Object arg,
			  Lisp_Object oldval)
{
  Emacs_Color col;

  CHECK_STRING (arg);
  if (!FRAME_TERMINAL (f)->defined_color_hook (f, SSDATA (arg),
					       &col, true, false))
    error ("Undefined color: %s", SSDATA (arg));

  FRAME_BACKGROUND_COLOR (f) = col.pixel;
  update_face_from_frame_parameter (f, Qbackground_color, arg);
  if (FRAME_VISIBLE_P (f))
    SET_FRAME_GARBAGED (f);
}

static void
web_set_cursor_color (struct frame *f, Lisp_Object arg,
		      Lisp_Object oldval)
{
  Emacs_Color col;

  CHECK_STRING (arg);
  if (!FRAME_TERMINAL (f)->defined_color_hook (f, SSDATA (arg),
					       &col, true, false))
    error ("Undefined color: %s", SSDATA (arg));

  FRAME_CURSOR_COLOR (f) = col.pixel;
  FRAME_X_OUTPUT (f)->cursor_foreground_color = FRAME_BACKGROUND_COLOR (f);

  if (FRAME_VISIBLE_P (f))
    {
      gui_update_cursor (f, false);
      gui_update_cursor (f, true);
    }
}


static void
web_set_cursor_type (struct frame *f, Lisp_Object arg,
		     Lisp_Object oldval)
{
  set_frame_cursor_types (f, arg);
}

static void
web_set_internal_border_width (struct frame *f, Lisp_Object arg,
			       Lisp_Object oldval)
{
  int old_width = FRAME_INTERNAL_BORDER_WIDTH (f);
  int new_width = check_int_nonnegative (arg);

  if (new_width == old_width)
    return;

  f->internal_border_width = new_width;
  adjust_frame_size (f, -1, -1, 3, 0, Qinternal_border_width);
  SET_FRAME_GARBAGED (f);
}

static void
web_set_menu_bar_lines (struct frame *f, Lisp_Object value,
			Lisp_Object oldval)
{
  /* Web frames don't have an external menu bar.  Allocate text-area
     lines for the menu bar when requested (same as terminal Emacs).  */
  if (FRAME_TOOLTIP_P (f))
    return;

  int nlines;
  if (TYPE_RANGED_FIXNUMP (int, value))
    nlines = XFIXNUM (value);
  else
    nlines = 0;

  if (nlines != FRAME_MENU_BAR_LINES (f))
    {
      FRAME_MENU_BAR_LINES (f) = nlines;
      FRAME_MENU_BAR_HEIGHT (f) = nlines * FRAME_LINE_HEIGHT (f);
      adjust_frame_glyphs (f);
      SET_FRAME_GARBAGED (f);
    }
}

void
web_change_tab_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TAB_BAR_HEIGHT (f);
  int lines = height / unit;

  if (lines == 0 && height != 0)
    lines = 1;

  fset_redisplay (f);

  FRAME_TAB_BAR_HEIGHT (f) = height;
  FRAME_TAB_BAR_LINES (f) = lines;
  store_frame_param (f, Qtab_bar_lines, make_fixnum (lines));

  if (FRAME_TAB_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }

  if ((height < old_height) && WINDOWP (f->tab_bar_window))
    clear_glyph_matrix (XWINDOW (f->tab_bar_window)->current_matrix);

  if (!f->tab_bar_resized)
    {
      Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtab_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtab_bar_lines);
      f->tab_bar_resized = f->tab_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtab_bar_lines);

  adjust_frame_glyphs (f);

  /* Do NOT call SET_FRAME_GARBAGED here.  On the web backend, a
     garbaged frame triggers redisplay_internal to clear glyph matrices
     via adjust_frame_glyphs with fonts_changed=true.  This loses the
     cached mode-line height, causing estimate_mode_line_height to
     return a value that differs from the actual rendered height (e.g.
     when doom-modeline uses :height 1.1 faces).  The mismatch sets
     fonts_changed again, creating an infinite redisplay retry loop.

     Using fset_redisplay instead triggers a thorough redisplay without
     clearing matrices, which avoids the estimate/actual oscillation.  */
  fset_redisplay (f);
}

static void
web_set_tab_bar_lines (struct frame *f, Lisp_Object value,
		       Lisp_Object oldval)
{
  if (FRAME_TOOLTIP_P (f) || FRAME_MINIBUF_ONLY_P (f))
    return;

  int olines = FRAME_TAB_BAR_LINES (f);
  int nlines;

  if (RANGED_FIXNUMP (0, value, INT_MAX))
    nlines = XFIXNAT (value);
  else
    nlines = 0;

  if (nlines != olines && (olines == 0 || nlines == 0))
    web_change_tab_bar_height (f, nlines * FRAME_LINE_HEIGHT (f));
}

void
web_change_tool_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TOOL_BAR_HEIGHT (f);
  int lines = (height + unit - 1) / unit;
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);

  fset_redisplay (f);

  FRAME_TOOL_BAR_HEIGHT (f) = height;
  FRAME_TOOL_BAR_LINES (f) = lines;
  store_frame_param (f, Qtool_bar_lines, make_fixnum (lines));

  if (FRAME_TOOL_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }

  if ((height < old_height) && WINDOWP (f->tool_bar_window))
    clear_glyph_matrix (XWINDOW (f->tool_bar_window)->current_matrix);

  if (!f->tool_bar_resized)
    {
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtool_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtool_bar_lines);
      f->tool_bar_resized = f->tool_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtool_bar_lines);

  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
web_set_tool_bar_lines (struct frame *f, Lisp_Object value,
			Lisp_Object oldval)
{
  if (FRAME_TOOLTIP_P (f) || FRAME_MINIBUF_ONLY_P (f))
    return;

  int nlines;
  if (RANGED_FIXNUMP (0, value, INT_MAX))
    nlines = XFIXNAT (value);
  else
    nlines = 0;

  web_change_tool_bar_height (f, nlines * FRAME_LINE_HEIGHT (f));
}

static void
web_explicitly_set_name (struct frame *f, Lisp_Object arg,
			 Lisp_Object oldval)
{
  if (!NILP (arg))
    {
      CHECK_STRING (arg);
      fset_name (f, arg);
      f->explicit_name = true;
    }
  else
    {
      fset_name (f, Vinvocation_name);
      f->explicit_name = false;
    }
}

static void
web_set_title (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  if (STRINGP (arg))
    fset_name (f, arg);
  else if (NILP (arg))
    fset_name (f, Vinvocation_name);
}


/* Frame parameter handler array.  This must have the same number of
   entries as the PGTK/X backends, with entries in the same order
   matching the frame_parms table in frame.c.  */

frame_parm_handler web_frame_parm_handlers[] =
  {
    gui_set_autoraise,			/* autoraise */
    gui_set_autolower,			/* autolower */
    web_set_background_color,		/* background-color */
    NULL,				/* border-color (no window border) */
    gui_set_border_width,		/* border-width */
    web_set_cursor_color,		/* cursor-color */
    web_set_cursor_type,		/* cursor-type */
    gui_set_font,			/* font */
    web_set_foreground_color,		/* foreground-color */
    NULL,				/* icon-name (not applicable) */
    NULL,				/* icon-type (not applicable) */
    NULL,				/* child-frame-border-width */
    web_set_internal_border_width,	/* internal-border-width */
    gui_set_right_divider_width,	/* right-divider-width */
    gui_set_bottom_divider_width,	/* bottom-divider-width */
    web_set_menu_bar_lines,		/* menu-bar-lines */
    NULL,				/* mouse-color (browser handles) */
    web_explicitly_set_name,		/* name */
    gui_set_scroll_bar_width,		/* scroll-bar-width */
    gui_set_scroll_bar_height,		/* scroll-bar-height */
    web_set_title,			/* title */
    gui_set_unsplittable,		/* unsplittable */
    gui_set_vertical_scroll_bars,	/* vertical-scroll-bars */
    gui_set_horizontal_scroll_bars,	/* horizontal-scroll-bars */
    gui_set_visibility,			/* visibility */
    web_set_tab_bar_lines,		/* tab-bar-lines */
    web_set_tool_bar_lines,		/* tool-bar-lines */
    NULL,				/* scroll-bar-foreground */
    NULL,				/* scroll-bar-background */
    gui_set_screen_gamma,		/* screen-gamma */
    gui_set_line_spacing,		/* line-spacing */
    gui_set_left_fringe,		/* left-fringe */
    gui_set_right_fringe,		/* right-fringe */
    NULL,				/* wait-for-wm */
    gui_set_fullscreen,			/* fullscreen */
    gui_set_font_backend,		/* font-backend */
    gui_set_alpha,			/* alpha */
    NULL,				/* sticky (not applicable) */
    NULL,				/* tool-bar-position */
    NULL,				/* inhibit-double-buffering */
    NULL,				/* undecorated (not applicable) */
    NULL,				/* parent-frame */
    NULL,				/* skip-taskbar */
    NULL,				/* no-focus-on-map */
    NULL,				/* no-accept-focus */
    NULL,				/* z-group */
    NULL,				/* override-redirect */
    gui_set_no_special_glyphs,		/* no-special-glyphs */
    gui_set_alpha_background,		/* alpha-background */
    gui_set_borders_respect_alpha_background, /* borders-respect-alpha-background */
    NULL,
  };


/* Unwind protect for frame creation.  */
static void
do_unwind_create_frame (Lisp_Object frame)
{
  struct frame *f = XFRAME (frame);

  if (!FRAME_LIVE_P (f))
    return;

  if (NILP (Fmemq (frame, Vframe_list)))
    {
      xfree (f->output_data.web);
      f->output_data.web = NULL;
      free_glyphs (f);
    }
}


DEFUN ("x-create-frame", Fx_create_frame, Sx_create_frame, 1, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object parms)
{
  struct frame *f;
  Lisp_Object frame, tem, name;
  struct web_display_info *dpyinfo = NULL;
  struct kboard *kb;
  specpdl_ref count = SPECPDL_INDEX ();

  parms = Fcopy_alist (parms);

  Vx_resource_name = Vinvocation_name;

  tem = gui_display_get_arg (dpyinfo, parms, Qterminal, 0, 0,
			     RES_TYPE_STRING);
  if (BASE_EQ (tem, Qunbound))
    tem = Qnil;

  /* Get display info.  For web, there's only one.  */
  dpyinfo = x_display_list;
  if (!dpyinfo)
    error ("Web display not initialized");

  kb = dpyinfo->terminal->kboard;

  if (!dpyinfo->terminal->name)
    error ("Terminal is not live, can't create new frames on it");

  name = gui_display_get_arg (dpyinfo, parms, Qname, 0, 0,
			      RES_TYPE_STRING);
  if (!STRINGP (name)
      && !BASE_EQ (name, Qunbound)
      && !NILP (name))
    error ("Invalid frame name--not a string or nil");

  if (STRINGP (name))
    Vx_resource_name = name;

  /* Create the frame.  */
  tem = gui_display_get_arg (dpyinfo, parms, Qminibuffer,
			     "minibuffer", "Minibuffer",
			     RES_TYPE_SYMBOL);
  if (EQ (tem, Qnone) || NILP (tem))
    f = make_frame_without_minibuffer (Qnil, kb, Qnil);
  else if (EQ (tem, Qonly))
    f = make_minibuffer_frame ();
  else if (WINDOWP (tem))
    f = make_frame_without_minibuffer (tem, kb, Qnil);
  else
    f = make_frame (true);

  XSETFRAME (frame, f);

  f->terminal = dpyinfo->terminal;
  f->output_method = output_web;
  f->output_data.web = xzalloc (sizeof *f->output_data.web);
  FRAME_FONTSET (f) = -1;
  FRAME_X_OUTPUT (f)->white_relief.pixel = -1;
  FRAME_X_OUTPUT (f)->black_relief.pixel = -1;

  FRAME_DISPLAY_INFO (f) = dpyinfo;

  /* Protect frame during setup.  */
  record_unwind_protect (do_unwind_create_frame, frame);

  /* Set name.  */
  if (BASE_EQ (name, Qunbound) || NILP (name) || !STRINGP (name))
    {
      fset_name (f, Vinvocation_name);
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
      specbind (Qx_resource_name, name);
    }

  /* Initialize default colors.  */
  FRAME_FOREGROUND_COLOR (f) = 0xFFFFFF; /* white */
  FRAME_BACKGROUND_COLOR (f) = 0x000000; /* black */
  FRAME_CURSOR_COLOR (f) = 0xFFFFFF;     /* white */
  FRAME_X_OUTPUT (f)->cursor_foreground_color = 0x000000;
  FRAME_X_OUTPUT (f)->border_pixel = 0x000000;

  /* Register font driver and set default font.  */
  register_font_driver (&webfont_driver, f);

  gui_default_parameter (f, parms, Qfont_backend, Qnil,
			 "fontBackend", "FontBackend", RES_TYPE_STRING);

  FRAME_RIF (f)->default_font_parameter (f, parms);

  if (!FRAME_FONT (f))
    {
      /* No font driver available — set up hardcoded metrics so the
	 frame can at least display text.  */
      FRAME_COLUMN_WIDTH (f) = dpyinfo->default_char_width;
      FRAME_LINE_HEIGHT (f) = dpyinfo->default_char_height;
    }

  /* Set frame parameters.  */
  gui_default_parameter (f, parms, Qborder_width, make_fixnum (0),
			 "borderwidth", "BorderWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qinternal_border_width, make_fixnum (0),
			 "internalBorderWidth", "InternalBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qchild_frame_border_width, Qnil,
			 "childFrameBorderWidth", "childFrameBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbottom_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qvertical_scroll_bars, Qnil,
			 "verticalScrollBars", "VerticalScrollBars",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qhorizontal_scroll_bars, Qnil,
			 "horizontalScrollBars", "HorizontalScrollBars",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qforeground_color, build_string ("white"),
			 "foreground", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qbackground_color, build_string ("black"),
			 "background", "Background", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qcursor_color, build_string ("white"),
			 "cursorColor", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qmouse_color, build_string ("white"),
			 "pointerColor", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qline_spacing, Qnil,
			 "lineSpacing", "LineSpacing", RES_TYPE_NUMBER);
  /* Disable fringes — the web display renders text in a DOM grid
     with no fringe areas.  Zero fringes ensures mouse pixel
     coordinates match the text area directly.  */
  gui_default_parameter (f, parms, Qleft_fringe, make_fixnum (0),
			 "leftFringe", "LeftFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_fringe, make_fixnum (0),
			 "rightFringe", "RightFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qno_special_glyphs, Qnil,
			 NULL, NULL, RES_TYPE_BOOLEAN);

  /* Initialize faces.  */
  init_frame_faces (f);

  /* Adjust frame size.  */
  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 5, true,
		     Qx_create_frame_1);

  /* Menu/tab/tool bar.  */
  gui_default_parameter (f, parms, Qmenu_bar_lines,
			 make_fixnum (1),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtab_bar_lines,
			 NILP (Vtab_bar_mode)
			 ? make_fixnum (0) : make_fixnum (1),
			 NULL, NULL, RES_TYPE_NUMBER);
  /* Force tool bar off — the web backend cannot render images.  */
  gui_default_parameter (f, parms, Qtool_bar_lines,
			 make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtitle, Qnil,
			 "title", "Title", RES_TYPE_STRING);

  tem = gui_display_get_arg (dpyinfo, parms, Qunsplittable, 0, 0,
			     RES_TYPE_BOOLEAN);
  f->no_split = !BASE_EQ (tem, Qunbound) && !NILP (tem);

  f->terminal->reference_count++;

  /* Add to frame list.  */
  Vframe_list = Fcons (frame, Vframe_list);

  /* Make the frame visible.  */
  SET_FRAME_VISIBLE (f, true);
  SET_FRAME_ICONIFIED (f, false);
  FRAME_X_OUTPUT (f)->has_been_visible = true;

  /* Enable the 60Hz redisplay atimer now that a frame exists.  */
  web_frame_ready = true;

  return unbind_to (count, frame);
}


DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* Hide the current tooltip window, if there is any.  */)
  (void)
{
  return Qnil;
}

DEFUN ("x-show-tip", Fx_show_tip, Sx_show_tip, 1, 6, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object string, Lisp_Object frame, Lisp_Object parms,
   Lisp_Object timeout, Lisp_Object dx, Lisp_Object dy)
{
  return Qnil;
}

DEFUN ("x-open-connection", Fx_open_connection, Sx_open_connection, 1, 3, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object display, Lisp_Object xrm_string, Lisp_Object must_succeed)
{
  if (x_display_list)
    {
      if (!NILP (must_succeed))
	fatal ("A display is already open");
      else
	error ("A display is already open");
    }

  web_term_init ();
  return Qnil;
}

DEFUN ("x-close-connection", Fx_close_connection, Sx_close_connection, 1, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return Qnil;
}

DEFUN ("x-display-list", Fx_display_list, Sx_display_list, 0, 0, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (void)
{
  return Qnil;
}

DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return Qt;
}

DEFUN ("x-display-grayscale-p", Fx_display_grayscale_p, Sx_display_grayscale_p, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return Qt;
}

DEFUN ("x-display-pixel-width", Fx_display_pixel_width, Sx_display_pixel_width, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1920);
}

DEFUN ("x-display-pixel-height", Fx_display_pixel_height, Sx_display_pixel_height, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1080);
}

DEFUN ("x-display-planes", Fx_display_planes, Sx_display_planes, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (24);
}

DEFUN ("x-display-color-cells", Fx_display_color_cells, Sx_display_color_cells, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (16777216);
}

DEFUN ("x-display-visual-class", Fx_display_visual_class, Sx_display_visual_class, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return intern ("true-color");
}

DEFUN ("x-display-screens", Fx_display_screens, Sx_display_screens, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (1);
}

DEFUN ("x-display-mm-width", Fx_display_mm_width, Sx_display_mm_width, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (508);
}

DEFUN ("x-display-mm-height", Fx_display_mm_height, Sx_display_mm_height, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (286);
}

DEFUN ("x-display-backing-store", Fx_display_backing_store, Sx_display_backing_store, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return intern ("not-useful");
}

DEFUN ("x-display-save-under", Fx_display_save_under, Sx_display_save_under, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return Qnil;
}

DEFUN ("x-server-max-request-size", Fx_server_max_request_size, Sx_server_max_request_size, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return make_fixnum (65535);
}


/* Called from frame.c.  */
struct web_display_info *
check_x_display_info (Lisp_Object object)
{
  if (!x_display_list)
    error ("Web display not initialized");
  return x_display_list;
}

/* Called from frame.c.  */
void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  /* Can't warp the mouse in a browser.  */
}

/* Called from keyboard.c.  */
char *
get_keysym_name (int keysym)
{
  static char buf[32];
  if (keysym < 128 && keysym > 32)
    {
      buf[0] = (char) keysym;
      buf[1] = '\0';
    }
  else
    snprintf (buf, sizeof buf, "key-%d", keysym);
  return buf;
}


DEFUN ("xw-color-defined-p", Fxw_color_defined_p, Sxw_color_defined_p, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color col;
  struct frame *f = decode_window_system_frame (frame);

  CHECK_STRING (color);
  if (FRAME_TERMINAL (f)->defined_color_hook (f, SSDATA (color),
					      &col, false, false))
    return Qt;
  return Qnil;
}

DEFUN ("xw-color-values", Fxw_color_values, Sxw_color_values, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  Emacs_Color col;
  struct frame *f = decode_window_system_frame (frame);

  CHECK_STRING (color);
  if (FRAME_TERMINAL (f)->defined_color_hook (f, SSDATA (color),
					      &col, false, false))
    return list3i (col.red, col.green, col.blue);
  return Qnil;
}


/* Base64 encoding table.  */
static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode binary data to base64.  Returns malloc'd string; caller frees.  */
static char *
base64_encode_alloc (const unsigned char *data, size_t len, size_t *out_len)
{
  size_t olen = 4 * ((len + 2) / 3);
  char *out = xmalloc (olen + 1);
  char *p = out;
  size_t i;

  for (i = 0; i + 2 < len; i += 3)
    {
      *p++ = b64_table[(data[i] >> 2) & 0x3F];
      *p++ = b64_table[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
      *p++ = b64_table[((data[i+1] & 0xF) << 2) | (data[i+2] >> 6)];
      *p++ = b64_table[data[i+2] & 0x3F];
    }
  if (i < len)
    {
      *p++ = b64_table[(data[i] >> 2) & 0x3F];
      if (i + 1 < len)
	{
	  *p++ = b64_table[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
	  *p++ = b64_table[(data[i+1] & 0xF) << 2];
	}
      else
	{
	  *p++ = b64_table[(data[i] & 0x3) << 4];
	  *p++ = '=';
	}
      *p++ = '=';
    }
  *p = '\0';
  *out_len = p - out;
  return out;
}

DEFUN ("web-load-font", Fweb_load_font, Sweb_load_font, 2, 2, 0,
       doc: /* Load a font file into the web display browser.
NAME is the font family name to register (a string).
FILE is the path to a .ttf or .otf font file.
The font is base64-encoded and sent to the browser via the FontFace API.  */)
  (Lisp_Object name, Lisp_Object file)
{
  CHECK_STRING (name);
  CHECK_STRING (file);

  struct web_display_info *dpyinfo = x_display_list;
  if (!dpyinfo)
    error ("Web display not initialized");

  /* Expand and validate file path.  */
  file = Fexpand_file_name (file, Qnil);
  const char *path = SSDATA (file);

  /* Read the font file.  */
  int fd = emacs_open (path, O_RDONLY, 0);
  if (fd < 0)
    error ("Cannot open font file: %s", path);

  struct stat st;
  if (fstat (fd, &st) < 0 || st.st_size == 0)
    {
      emacs_close (fd);
      error ("Cannot stat font file: %s", path);
    }

  size_t file_size = st.st_size;
  unsigned char *buf = xmalloc (file_size);
  size_t total = 0;
  while (total < file_size)
    {
      ssize_t n = read (fd, buf + total, file_size - total);
      if (n <= 0)
	{
	  xfree (buf);
	  emacs_close (fd);
	  error ("Error reading font file: %s", path);
	}
      total += n;
    }
  emacs_close (fd);

  /* Base64-encode.  */
  size_t b64_len;
  char *b64 = base64_encode_alloc (buf, file_size, &b64_len);
  xfree (buf);

  /* Write the load_font JSON message.  */
  const char *font_name = SSDATA (name);
  WR_LIT (dpyinfo, "{\"type\":\"load_font\",\"name\":");
  web_write_json_string (dpyinfo, font_name, (int) SBYTES (name));
  WR_LIT (dpyinfo, ",\"data\":\"");
  web_write_str (dpyinfo, b64, b64_len);
  WR_LIT (dpyinfo, "\"}\n");
  xfree (b64);

  /* Flush immediately.  */
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      web_async_enqueue_output (&dpyinfo->async,
				dpyinfo->write_buf, dpyinfo->write_buf_len);
      dpyinfo->write_buf_len = 0;
      web_frame_output_wake (&dpyinfo->async);
    }
  else
#endif
    web_write_flush (dpyinfo);

  return Qnil;
}

static void
web_flush_control_message (struct web_display_info *dpyinfo)
{
#ifdef HAVE_PTHREAD
  if (dpyinfo->async_enabled)
    {
      web_async_enqueue_output (&dpyinfo->async,
				dpyinfo->write_buf, dpyinfo->write_buf_len);
      dpyinfo->write_buf_len = 0;
      web_frame_output_wake (&dpyinfo->async);
    }
  else
#endif
    web_write_flush (dpyinfo);
}

DEFUN ("web-eval-javascript", Fweb_eval_javascript, Sweb_eval_javascript,
       1, 1, 0,
       doc: /* Evaluate CODE in the web display browser.
CODE is sent to the browser process and evaluated there.  This is a
one-way display primitive; use it for browser widgets and diagnostics.  */)
  (Lisp_Object code)
{
  CHECK_STRING (code);

  struct web_display_info *dpyinfo = x_display_list;
  if (!dpyinfo)
    error ("Web display not initialized");

  Lisp_Object encoded = ENCODE_UTF_8 (code);
  WR_LIT (dpyinfo, "{\"type\":\"eval\",\"code\":");
  web_write_json_string (dpyinfo, SSDATA (encoded), (int) SBYTES (encoded));
  WR_LIT (dpyinfo, "}\n");
  web_flush_control_message (dpyinfo);

  return Qnil;
}

void
syms_of_webfns (void)
{
  defsubr (&Sweb_eval_javascript);
  defsubr (&Sweb_load_font);
  defsubr (&Sxw_color_defined_p);
  defsubr (&Sxw_color_values);
  defsubr (&Sx_hide_tip);
  defsubr (&Sx_show_tip);
  defsubr (&Sx_create_frame);
  defsubr (&Sx_open_connection);
  defsubr (&Sx_close_connection);
  defsubr (&Sx_display_list);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sx_display_pixel_width);
  defsubr (&Sx_display_pixel_height);
  defsubr (&Sx_display_planes);
  defsubr (&Sx_display_color_cells);
  defsubr (&Sx_display_visual_class);
  defsubr (&Sx_display_screens);
  defsubr (&Sx_display_mm_width);
  defsubr (&Sx_display_mm_height);
  defsubr (&Sx_display_backing_store);
  defsubr (&Sx_display_save_under);
  defsubr (&Sx_server_max_request_size);
}
