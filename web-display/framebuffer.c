/* In-memory framebuffer for Emacs web display proxy.  */

#include "framebuffer.h"
#include "protocol.h"
#include "websocket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct framebuffer *
fb_create (int cols, int rows, int cell_w, int cell_h)
{
  struct framebuffer *fb = calloc (1, sizeof *fb);
  if (!fb)
    return NULL;

  fb->cols = cols;
  fb->rows = rows;
  fb->cell_w = cell_w;
  fb->cell_h = cell_h;
  fb->pixel_w = cols * cell_w;
  fb->pixel_h = rows * cell_h;
  fb->default_bg = 0x000000;

  fb->cells = calloc ((size_t)cols * rows, sizeof (struct fb_cell));
  if (!fb->cells)
    {
      free (fb);
      return NULL;
    }

  /* Initialize all cells to space with default colors.  */
  for (int i = 0; i < cols * rows; i++)
    {
      fb->cells[i].codepoint = ' ';
      fb->cells[i].fg = 0xFFFFFF;
      fb->cells[i].bg = 0x000000;
      fb->cells[i].flags = 0;
    }

  return fb;
}

void
fb_destroy (struct framebuffer *fb)
{
  if (fb)
    {
      free (fb->cells);
      free (fb);
    }
}

int
fb_resize (struct framebuffer *fb, int new_cols, int new_rows)
{
  struct fb_cell *new_cells = calloc ((size_t)new_cols * new_rows,
                                     sizeof (struct fb_cell));
  if (!new_cells)
    return -1;

  /* Initialize new cells.  */
  for (int i = 0; i < new_cols * new_rows; i++)
    {
      new_cells[i].codepoint = ' ';
      new_cells[i].fg = 0xFFFFFF;
      new_cells[i].bg = fb->default_bg;
      new_cells[i].flags = 0;
    }

  /* Copy overlapping region.  */
  int copy_cols = (new_cols < fb->cols) ? new_cols : fb->cols;
  int copy_rows = (new_rows < fb->rows) ? new_rows : fb->rows;
  for (int r = 0; r < copy_rows; r++)
    memcpy (new_cells + r * new_cols,
            fb->cells + r * fb->cols,
            (size_t)copy_cols * sizeof (struct fb_cell));

  free (fb->cells);
  fb->cells = new_cells;
  fb->cols = new_cols;
  fb->rows = new_rows;
  fb->pixel_w = new_cols * fb->cell_w;
  fb->pixel_h = new_rows * fb->cell_h;
  return 0;
}

void
fb_clear_rect (struct framebuffer *fb, int x, int y, int w, int h,
               uint32_t color)
{
  /* Convert pixel coords to cell coords.  */
  int c0 = x / fb->cell_w;
  int r0 = y / fb->cell_h;
  int c1 = (x + w + fb->cell_w - 1) / fb->cell_w;
  int r1 = (y + h + fb->cell_h - 1) / fb->cell_h;

  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  if (c1 > fb->cols) c1 = fb->cols;
  if (r1 > fb->rows) r1 = fb->rows;

  for (int r = r0; r < r1; r++)
    for (int c = c0; c < c1; c++)
      {
        struct fb_cell *cell = &fb->cells[r * fb->cols + c];
        cell->codepoint = ' ';
        cell->fg = 0xFFFFFF;
        cell->bg = color;
        cell->flags = 0;
      }
}

/* Decode one UTF-8 character.  Returns codepoint and advances *p.
   Returns 0xFFFD on error.  */
static uint32_t
decode_utf8 (const uint8_t **p, const uint8_t *end)
{
  const uint8_t *s = *p;
  if (s >= end)
    return 0;

  uint32_t cp;
  int len;

  if (s[0] < 0x80)
    { cp = s[0]; len = 1; }
  else if ((s[0] & 0xE0) == 0xC0)
    { cp = s[0] & 0x1F; len = 2; }
  else if ((s[0] & 0xF0) == 0xE0)
    { cp = s[0] & 0x0F; len = 3; }
  else if ((s[0] & 0xF8) == 0xF0)
    { cp = s[0] & 0x07; len = 4; }
  else
    { *p = s + 1; return 0xFFFD; }

  if (s + len > end)
    { *p = end; return 0xFFFD; }

  for (int i = 1; i < len; i++)
    {
      if ((s[i] & 0xC0) != 0x80)
        { *p = s + i; return 0xFFFD; }
      cp = (cp << 6) | (s[i] & 0x3F);
    }

  *p = s + len;
  return cp;
}

void
fb_write_glyphs (struct framebuffer *fb, int x, int y,
                 const uint8_t *utf8, int len,
                 uint32_t fg, uint32_t bg, uint8_t flags)
{
  int col = x / fb->cell_w;
  int row = y / fb->cell_h;

  if (row < 0 || row >= fb->rows)
    return;

  const uint8_t *p = utf8;
  const uint8_t *end = utf8 + len;

  while (p < end && col < fb->cols)
    {
      uint32_t cp = decode_utf8 (&p, end);
      if (col >= 0)
        {
          struct fb_cell *cell = &fb->cells[row * fb->cols + col];
          cell->codepoint = cp;
          cell->fg = fg;
          cell->bg = bg;
          cell->flags = flags;
        }
      col++;
    }
}

void
fb_scroll_rect (struct framebuffer *fb, int x, int y, int w, int h, int dy)
{
  int c0 = x / fb->cell_w;
  int r0 = y / fb->cell_h;
  int c1 = (x + w) / fb->cell_w;
  int r1 = (y + h) / fb->cell_h;

  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  if (c1 > fb->cols) c1 = fb->cols;
  if (r1 > fb->rows) r1 = fb->rows;

  int drows = dy / fb->cell_h;
  if (drows == 0)
    return;

  int width = c1 - c0;
  if (width <= 0)
    return;

  if (drows > 0)
    {
      /* Scroll down: copy rows from bottom to top.  */
      for (int r = r1 - 1; r >= r0 + drows; r--)
        memcpy (&fb->cells[r * fb->cols + c0],
                &fb->cells[(r - drows) * fb->cols + c0],
                (size_t)width * sizeof (struct fb_cell));
      /* Clear exposed rows at top.  */
      for (int r = r0; r < r0 + drows && r < r1; r++)
        for (int c = c0; c < c1; c++)
          {
            struct fb_cell *cell = &fb->cells[r * fb->cols + c];
            cell->codepoint = ' ';
            cell->fg = 0xFFFFFF;
            cell->bg = fb->default_bg;
            cell->flags = 0;
          }
    }
  else
    {
      /* Scroll up: copy rows from top to bottom.  */
      int adrows = -drows;
      for (int r = r0; r < r1 - adrows; r++)
        memcpy (&fb->cells[r * fb->cols + c0],
                &fb->cells[(r + adrows) * fb->cols + c0],
                (size_t)width * sizeof (struct fb_cell));
      /* Clear exposed rows at bottom.  */
      for (int r = r1 - adrows; r < r1; r++)
        for (int c = c0; c < c1; c++)
          {
            struct fb_cell *cell = &fb->cells[r * fb->cols + c];
            cell->codepoint = ' ';
            cell->fg = 0xFFFFFF;
            cell->bg = fb->default_bg;
            cell->flags = 0;
          }
    }
}

/* Encode a single UTF-8 character into buf.  Returns bytes written.  */
static int
encode_utf8 (uint32_t cp, uint8_t *buf)
{
  if (cp < 0x80)
    {
      buf[0] = (uint8_t)cp;
      return 1;
    }
  else if (cp < 0x800)
    {
      buf[0] = 0xC0 | (uint8_t)(cp >> 6);
      buf[1] = 0x80 | (uint8_t)(cp & 0x3F);
      return 2;
    }
  else if (cp < 0x10000)
    {
      buf[0] = 0xE0 | (uint8_t)(cp >> 12);
      buf[1] = 0x80 | (uint8_t)((cp >> 6) & 0x3F);
      buf[2] = 0x80 | (uint8_t)(cp & 0x3F);
      return 3;
    }
  else
    {
      buf[0] = 0xF0 | (uint8_t)(cp >> 18);
      buf[1] = 0x80 | (uint8_t)((cp >> 12) & 0x3F);
      buf[2] = 0x80 | (uint8_t)((cp >> 6) & 0x3F);
      buf[3] = 0x80 | (uint8_t)(cp & 0x3F);
      return 4;
    }
}

void
fb_replay (struct framebuffer *fb, struct ws_server *srv, int client_idx)
{
  /* Send frame size first so the browser knows the canvas dimensions.  */
  {
    uint8_t fs_buf[11];
    size_t fs_len = proto_encode_frame_size (fs_buf, 0,
                                             (uint16_t)fb->pixel_w,
                                             (uint16_t)fb->pixel_h,
                                             (uint16_t)fb->cell_w,
                                             (uint16_t)fb->cell_h);
    ws_send_binary (srv, client_idx, fs_buf, fs_len);
  }

  /* Send row by row.  For each row, find runs of cells with the same
     fg/bg/flags and send them as single DRAW_GLYPHS messages.  */
  uint8_t msg_buf[16 + 4096]; /* header + up to 4096 bytes of utf8 */

  for (int r = 0; r < fb->rows; r++)
    {
      int c = 0;
      while (c < fb->cols)
        {
          struct fb_cell *start = &fb->cells[r * fb->cols + c];
          uint32_t fg = start->fg;
          uint32_t bg = start->bg;
          uint8_t flags = start->flags;

          /* Collect run of same-styled cells.  */
          uint8_t utf8_buf[4096];
          int utf8_len = 0;
          int run_start = c;

          while (c < fb->cols)
            {
              struct fb_cell *cell = &fb->cells[r * fb->cols + c];
              if (cell->fg != fg || cell->bg != bg || cell->flags != flags)
                break;
              int n = encode_utf8 (cell->codepoint, utf8_buf + utf8_len);
              if (utf8_len + n > 4000)
                break; /* avoid overflow */
              utf8_len += n;
              c++;
            }

          if (utf8_len > 0)
            {
              size_t mlen = proto_encode_draw_glyphs (
                msg_buf,
                (uint16_t)(run_start * fb->cell_w),
                (uint16_t)(r * fb->cell_h),
                fg, bg, flags,
                utf8_buf, (uint16_t)utf8_len);
              ws_send_binary (srv, client_idx, msg_buf, mlen);
            }
        }
    }

  /* Send a flush.  */
  uint8_t flush_buf[1];
  size_t flen = proto_encode_flush (flush_buf);
  ws_send_binary (srv, client_idx, flush_buf, flen);
}
