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

#ifndef WEBTERM_H
#define WEBTERM_H

#include "dispextern.h"
#include "frame.h"
#include "character.h"
#include "font.h"
#include "sysselect.h"

#ifdef HAVE_PTHREAD
#include "web_event_loop.h"
#endif

/* Forward declarations.  */
struct web_display_info;
struct web_output;

struct web_bitmap_record
{
  char *file;
  int refcount;
  int height, width, depth;
};

#define WEB_MAX_WINDOWS 12
#define WEB_MAX_LINES 160
#define WEB_MAX_RUNS 32
#define WEB_MAX_FACES 512
#define WEB_MAX_SCROLLS 64
#define WEB_MAX_CLEAR_AREAS 64
#define WEB_RUN_TEXT_CAP 1024

/* Information about the web display we are connected to.  */

struct web_display_info
{
  /* Chain of all web_display_info structures.  */
  struct web_display_info *next;

  /* The terminal corresponding to this display.  */
  struct terminal *terminal;

  /* This is a cons cell of the form (NAME . FONT-LIST-CACHE).  */
  Lisp_Object name_list_element;

  /* Number of frames on this display.  */
  int reference_count;

  /* File descriptor to display proxy process.  */
  int proxy_fd;

  /* PID of display proxy process.  */
  pid_t proxy_pid;

  /* WebSocket port number.  */
  int port;

  /* Write buffer for batching draw commands between flushes.  */
  unsigned char *write_buf;
  int write_buf_len;
  int write_buf_size;

  /* Read buffer for input events from proxy.  */
  unsigned char *read_buf;
  int read_buf_len;
  int read_buf_size;

  /* Color map: alist of (NAME . PIXEL).  */
  Lisp_Object color_map;

  /* Minimum width over all characters in all fonts in font_table.  */
  int smallest_char_width;

  /* Minimum font height over all fonts in font_table.  */
  int smallest_font_height;

  /* The number of fonts loaded.  */
  int n_fonts;

  /* Number of planes on this display (always 24 for web).  */
  int n_planes;

  /* Information about the range of text currently shown in
     mouse-face.  */
  Mouse_HLInfo mouse_highlight;

  /* The frame with current input focus.  */
  struct frame *x_focus_frame;

  /* The last frame mentioned in a FocusIn or FocusOut event.  */
  struct frame *x_focus_event_frame;

  /* The frame where the mouse was last time we reported a mouse event.  */
  struct frame *last_mouse_frame;

  /* The frame where the mouse was last time we reported a mouse motion.  */
  struct frame *last_mouse_motion_frame;

  /* Position where the mouse was last time we reported a motion.  */
  int last_mouse_motion_x;
  int last_mouse_motion_y;

  /* Where the mouse was last time we reported a mouse position.  */
  NativeRectangle last_mouse_glyph;

  /* Time of last mouse movement.  */
  Time last_mouse_movement_time;

  /* The frame where the mouse was last time we reported a mouse position.  */
  struct frame *last_mouse_glyph_frame;

  /* Mask of things that cause the mouse to be grabbed.  */
  int grabbed;

  /* The frame currently highlighted for mouse-face.  */
  struct frame *highlight_frame;

  /* DPI resolution of this screen.  */
  double resx, resy;

  /* Root window (X-compat stub).  */
  Window root_window;

  /* Xism (X-compat stub).  */
  int rdb;

  /* The invisible cursor used for pointer blanking.  */
  Emacs_Cursor invisible_cursor;

  /* Emacs bitmap-id of the default icon bitmap.  */
  ptrdiff_t icon_bitmap_id;

  /* Default character dimensions.  */
  int default_char_width;
  int default_char_height;

  /* Bitmap data.  */
  struct web_bitmap_record *bitmaps;
  ptrdiff_t bitmaps_size;
  ptrdiff_t bitmaps_last;

  int color_p;

  /* Timing: nanosecond timestamp of last input arrival.  */
  uint64_t last_input_ns;

  /* JSON frame state — accumulates semantic data during redisplay,
     serialized to NDJSON at flush time.  */
  struct json_frame_state {
    struct json_window {
      EMACS_INT id;
      int x, y, w, h;          /* char-cell units */
      int px, py, pw, ph;      /* frame-relative pixel geometry */
      struct json_line {
        int row_index;
        int pixel_y, pixel_h;  /* window-relative pixel geometry */
        int seq;               /* capture order within the cycle; on
                                  pixel overlap the later capture wins
                                  (scrolls reuse row indices mid-cycle,
                                  so early captures can be stale) */
        bool mode_line_p, continued_p, truncated_left_p, truncated_right_p;
        bool complete;   /* Set by after_update_window_line.  */
        struct json_run {
          int face_id;
          char text[WEB_RUN_TEXT_CAP];
          int text_len;
          int img_id;   /* >0 if this run is an image glyph */
          int img_w;    /* image width in pixels */
          int img_h;    /* image height in pixels */
        } runs[WEB_MAX_RUNS];
        int nruns;
      } lines[WEB_MAX_LINES];
      int nlines;
      struct { int row, col, type; bool active, visible; } cursor;
      bool has_cursor;
      bool active;
      bool has_complete_lines;  /* Any after_update_window_line calls?  */
      bool is_menu_bar;         /* True for the menu bar pseudo-window.  */
      bool is_minibuffer;       /* True for the minibuffer/echo-area window.  */
      char webview_url[512];    /* Buffer-local `web-webview-url' of the
                                   window's buffer; non-empty means the
                                   client overlays an iframe over the
                                   window body.  Always emitted (possibly
                                   empty) so stale overlays clear.  */
      int webview_ph;           /* Body pixel height, mode line excluded.  */
      char tldraw_id[64];       /* Buffer-local `web-tldraw-board-id' of the
                                   window's buffer; non-empty means the
                                   client overlays a tldraw whiteboard over
                                   the window body.  Always emitted (possibly
                                   empty) so stale boards clear.  */
    } windows[WEB_MAX_WINDOWS];
    int nwindows;

    int face_ids[WEB_MAX_FACES];        /* faces referenced this cycle */
    int nface_ids;

    int line_build_seq;        /* monotonically increasing capture
                                  counter for json_line.seq */

    struct {
      EMACS_INT window_id;
      int delta_rows;
      int current_row;
      int desired_row;
      int nrows;
      int delta_px;             /* desired_y - current_y: pixel shift of
                                   the moved block.  Row indices alone
                                   misplace moved lines when row heights
                                   vary (images).  */
    } scrolls[WEB_MAX_SCROLLS];
    int nscrolls;

    struct {
      int x, y, width, height;
      unsigned long bg;
    } clear_areas[WEB_MAX_CLEAR_AREAS];
    int nclear_areas;

    bool clear_pending;
    unsigned long clear_bg;

    struct json_window *current_window;
    struct json_line *current_line;
    int current_line_row;

    /* Track which image IDs have been sent to avoid resending.
       Stores the image spec hash per slot so we detect when an image
       cache slot is reused with a different image (after GC).  A zero
       hash means "not sent".  */
    #define WEB_IMG_SENT_MAX 2048
    unsigned int img_sent_hash[WEB_IMG_SENT_MAX];
  } json_state;

#ifdef HAVE_PTHREAD
  /* Async event loop state.  */
  struct web_async_state async;
  bool async_enabled;
#endif
};

/* Per-frame data for the web backend.  */

struct web_output
{
  /* Foreground and background colors.  */
  unsigned long foreground_color;
  unsigned long background_color;

  /* Cursor colors.  */
  unsigned long cursor_color;
  unsigned long cursor_foreground_color;

  /* Mouse color.  */
  unsigned long mouse_color;

  /* Border color.  */
  unsigned long border_pixel;

  /* Cursors.  */
  Emacs_Cursor current_cursor;
  Emacs_Cursor text_cursor;
  Emacs_Cursor nontext_cursor;
  Emacs_Cursor modeline_cursor;
  Emacs_Cursor hand_cursor;
  Emacs_Cursor hourglass_cursor;
  Emacs_Cursor horizontal_drag_cursor;
  Emacs_Cursor vertical_drag_cursor;
  Emacs_Cursor left_edge_cursor;
  Emacs_Cursor top_left_corner_cursor;
  Emacs_Cursor top_edge_cursor;
  Emacs_Cursor top_right_corner_cursor;
  Emacs_Cursor right_edge_cursor;
  Emacs_Cursor bottom_right_corner_cursor;
  Emacs_Cursor bottom_edge_cursor;
  Emacs_Cursor bottom_left_corner_cursor;
  Emacs_Cursor current_pointer;

  /* Window IDs (X-compat, used by frame.c).  */
  Window window_desc, parent_desc;
  char explicit_parent;

  /* The font in use.  */
  struct font *font;

  /* Baseline offset.  */
  int baseline_offset;

  /* If a fontset is specified for this frame instead of font, this
     value contains an ID of the fontset, else -1.  */
  int fontset;

  /* Icon bitmap index.  */
  ptrdiff_t icon_bitmap;

  int icon_top;
  int icon_left;

  /* Pointer to our display info struct.  */
  struct web_display_info *display_info;

  /* Canvas pixel dimensions.  */
  int pixel_width;
  int pixel_height;

  /* Whether this frame has been made visible.  */
  int has_been_visible;

  /* Relief GCs, colors etc.  */
  struct relief
  {
    unsigned long pixel;
  }
  black_relief, white_relief;

  /* The background for which the above relief colors were set up.  */
  unsigned long relief_background;

  /* Whether or not a relief background has been computed.  */
  bool relief_background_valid_p;
};


/* Macros for accessing per-frame data.  */

#define FRAME_X_OUTPUT(f)         ((f)->output_data.web)
#define FRAME_OUTPUT_DATA(f)      FRAME_X_OUTPUT (f)

#define FRAME_DISPLAY_INFO(f)     (FRAME_X_OUTPUT (f)->display_info)
#define FRAME_FOREGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->foreground_color)
#define FRAME_BACKGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->background_color)
#define FRAME_CURSOR_COLOR(f)     (FRAME_X_OUTPUT (f)->cursor_color)
#define FRAME_FONT(f)             (FRAME_X_OUTPUT (f)->font)
#define FRAME_FONTSET(f)          (FRAME_X_OUTPUT (f)->fontset)
#define FRAME_BASELINE_OFFSET(f)  (FRAME_X_OUTPUT (f)->baseline_offset)
#define FRAME_MENUBAR_HEIGHT(f)   0
#define FRAME_TOOLBAR_TOP_HEIGHT(f) 0
#define FRAME_TOOLBAR_BOTTOM_HEIGHT(f) 0
#define FRAME_TOOLBAR_HEIGHT(f)   0
#define FRAME_TOOLBAR_LEFT_WIDTH(f) 0
#define FRAME_TOOLBAR_RIGHT_WIDTH(f) 0
#define FRAME_TOOLBAR_WIDTH(f)    0

#define BLACK_PIX_DEFAULT(f)      0x000000
#define WHITE_PIX_DEFAULT(f)      0xFFFFFF

#define FRAME_DEFAULT_FACE(f)     FACE_FROM_ID_OR_NULL (f, DEFAULT_FACE_ID)

/* X-compatible aliases used by shared gui_* code.  */
#define FRAME_X_WINDOW(f)         0
#define FRAME_NATIVE_WINDOW(f)    0
#define FRAME_X_DISPLAY(f)        NULL

#define FIRST_CHAR_POSITION(f)				\
  (! (FRAME_HAS_VERTICAL_SCROLL_BARS_ON_LEFT (f)) ? 0	\
   : FRAME_SCROLL_BAR_COLS (f))

/* Turning a lisp vector value into a pointer to a struct scroll_bar.  */
#define XSCROLL_BAR(vec) ((struct scroll_bar *) XVECTOR (vec))


/* Public function declarations.  */

extern struct terminal *web_create_terminal (struct web_display_info *dpyinfo);
extern void web_term_init (void);
extern void syms_of_webterm (void);
extern void syms_of_webfns (void);
extern void syms_of_webfont (void);
#ifdef HAVE_VTERM
extern void syms_of_webvterm (void);
#endif

/* Write buffer API — used by webfns.c for load_font, image_data, etc.  */
extern void web_write_str (struct web_display_info *dpyinfo,
			   const char *s, int len);
extern void web_write_json_string (struct web_display_info *dpyinfo,
				   const char *s, int len);
extern void web_write_flush (struct web_display_info *dpyinfo);
#define WR_LIT(dp, s) web_write_str (dp, s, sizeof (s) - 1)

extern struct font_driver const webfont_driver;

extern struct web_display_info *web_display_info;

/* x_display_list is a poorly-named global used by all backends.  */
extern struct web_display_info *x_display_list;

extern frame_parm_handler web_frame_parm_handlers[];
extern void web_change_tab_bar_height (struct frame *, int);
extern void web_change_tool_bar_height (struct frame *, int);
extern bool web_async_active_p (void);
extern bool web_yield_due_p (void);
extern void web_process_pending_events (void);
extern void web_redisplay_and_publish (void);

/* Set to true once the first web frame is fully created.
   Guards the 60Hz atimer from calling Fredisplay during init.  */
extern bool web_frame_ready;

#endif /* WEBTERM_H */
