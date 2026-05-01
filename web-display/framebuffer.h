/* In-memory framebuffer for Emacs web display proxy.
   Stores a cell grid so new clients can be synced instantly.  */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

/* Flags per cell — match GLYPH_* from protocol.h.  */
#define CELL_BOLD          (1 << 0)
#define CELL_ITALIC        (1 << 1)
#define CELL_UNDERLINE     (1 << 2)
#define CELL_STRIKETHROUGH (1 << 3)
#define CELL_BOX           (1 << 4)

struct fb_cell
{
  uint32_t codepoint;  /* Unicode codepoint, 0 = empty */
  uint32_t fg;         /* foreground color (0xRRGGBB) */
  uint32_t bg;         /* background color (0xRRGGBB) */
  uint8_t  flags;      /* CELL_* flags */
};

struct framebuffer
{
  int cols, rows;

  /* Character cell dimensions in pixels.  */
  int cell_w, cell_h;

  /* Total pixel dimensions.  */
  int pixel_w, pixel_h;

  /* Default background color.  */
  uint32_t default_bg;

  /* Cell grid: rows * cols entries.  */
  struct fb_cell *cells;
};

/* Create a framebuffer with given dimensions.
   cell_w/cell_h are the pixel size of each character cell.
   Returns NULL on allocation failure.  */
struct framebuffer *fb_create (int cols, int rows, int cell_w, int cell_h);

/* Free a framebuffer.  */
void fb_destroy (struct framebuffer *fb);

/* Resize framebuffer.  Preserves content where possible.
   Returns 0 on success, -1 on failure.  */
int fb_resize (struct framebuffer *fb, int new_cols, int new_rows);

/* Clear a rectangular region (in pixel coords) to color.
   Converts to cell coords internally.  */
void fb_clear_rect (struct framebuffer *fb, int x, int y,
                    int w, int h, uint32_t color);

/* Write glyphs at pixel position.  utf8 is the text, len its byte length.
   fg/bg are colors, flags are CELL_* flags.  */
void fb_write_glyphs (struct framebuffer *fb, int x, int y,
                      const uint8_t *utf8, int len,
                      uint32_t fg, uint32_t bg, uint8_t flags);

/* Scroll a rectangle by dy pixels.  Clears exposed area to default_bg.  */
void fb_scroll_rect (struct framebuffer *fb, int x, int y,
                     int w, int h, int dy);

/* Send entire framebuffer as DRAW_GLYPHS + FLUSH to a WebSocket client.
   Uses ws_send_binary.  srv and client_idx are passed through.  */
struct ws_server;
void fb_replay (struct framebuffer *fb, struct ws_server *srv, int client_idx);

#endif /* FRAMEBUFFER_H */
