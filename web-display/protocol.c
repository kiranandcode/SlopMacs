/* Wire protocol encode/decode for Emacs web display backend.  */

#include "protocol.h"
#include <string.h>

/* Little-endian helpers.  */

static inline void
put_u16 (uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
}

static inline void
put_u32 (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline void
put_u64 (uint8_t *p, uint64_t v)
{
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (i * 8));
}

static inline void
put_i16 (uint8_t *p, int16_t v)
{
  put_u16 (p, (uint16_t)v);
}

static inline uint16_t
get_u16 (const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t
get_u32 (const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
    | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t
get_u64 (const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= (uint64_t)p[i] << (i * 8);
  return v;
}

static inline int16_t
get_i16 (const uint8_t *p)
{
  return (int16_t)get_u16 (p);
}


/* Encode functions.  */

size_t
proto_encode_clear_rect (uint8_t *buf, uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h, uint32_t color)
{
  buf[0] = OP_CLEAR_RECT;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u16 (buf + 5, w);
  put_u16 (buf + 7, h);
  put_u32 (buf + 9, color);
  return 13;
}

size_t
proto_encode_draw_glyphs (uint8_t *buf, uint16_t x, uint16_t y,
                          uint32_t fg, uint32_t bg, uint8_t flags,
                          const uint8_t *utf8, uint16_t len)
{
  buf[0] = OP_DRAW_GLYPHS;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u32 (buf + 5, fg);
  put_u32 (buf + 9, bg);
  buf[13] = flags;
  put_u16 (buf + 14, len);
  memcpy (buf + 16, utf8, len);
  return 16 + len;
}

size_t
proto_encode_draw_cursor (uint8_t *buf, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h, uint8_t type,
                          uint32_t color)
{
  buf[0] = OP_DRAW_CURSOR;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u16 (buf + 5, w);
  put_u16 (buf + 7, h);
  buf[9] = type;
  put_u32 (buf + 10, color);
  return 14;
}

size_t
proto_encode_scroll_rect (uint8_t *buf, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h, int16_t dy)
{
  buf[0] = OP_SCROLL_RECT;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u16 (buf + 5, w);
  put_u16 (buf + 7, h);
  put_i16 (buf + 9, dy);
  return 11;
}

size_t
proto_encode_fill_rect (uint8_t *buf, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h, uint32_t color)
{
  buf[0] = OP_FILL_RECT;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u16 (buf + 5, w);
  put_u16 (buf + 7, h);
  put_u32 (buf + 9, color);
  return 13;
}

size_t
proto_encode_flush (uint8_t *buf)
{
  buf[0] = OP_FLUSH;
  return 1;
}

size_t
proto_encode_frame_size (uint8_t *buf, uint16_t frame_id,
                         uint16_t w, uint16_t h,
                         uint16_t cw, uint16_t ch)
{
  buf[0] = OP_FRAME_SIZE;
  put_u16 (buf + 1, frame_id);
  put_u16 (buf + 3, w);
  put_u16 (buf + 5, h);
  put_u16 (buf + 7, cw);
  put_u16 (buf + 9, ch);
  return 11;
}

size_t
proto_encode_draw_image (uint8_t *buf, uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h,
                         const uint8_t *png, uint32_t len)
{
  buf[0] = OP_DRAW_IMAGE;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_u16 (buf + 5, w);
  put_u16 (buf + 7, h);
  put_u32 (buf + 9, len);
  memcpy (buf + 13, png, len);
  return 13 + len;
}

size_t
proto_encode_heartbeat (uint8_t *buf, uint64_t timestamp_ms)
{
  buf[0] = OP_HEARTBEAT;
  put_u64 (buf + 1, timestamp_ms);
  return 9;
}

size_t
proto_encode_key_event (uint8_t *buf, uint32_t keycode,
                        uint8_t modifiers, uint32_t utf32_char)
{
  buf[0] = OP_KEY_EVENT;
  put_u32 (buf + 1, keycode);
  buf[5] = modifiers;
  put_u32 (buf + 6, utf32_char);
  return 10;
}

size_t
proto_encode_mouse_button (uint8_t *buf, uint16_t x, uint16_t y,
                           uint8_t button, uint8_t pressed, uint8_t mods)
{
  buf[0] = OP_MOUSE_BUTTON;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  buf[5] = button;
  buf[6] = pressed;
  buf[7] = mods;
  return 8;
}

size_t
proto_encode_mouse_move (uint8_t *buf, uint16_t x, uint16_t y, uint8_t mods)
{
  buf[0] = OP_MOUSE_MOVE;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  buf[5] = mods;
  return 6;
}

size_t
proto_encode_scroll (uint8_t *buf, uint16_t x, uint16_t y,
                     int16_t dx, int16_t dy, uint8_t mods)
{
  buf[0] = OP_SCROLL;
  put_u16 (buf + 1, x);
  put_u16 (buf + 3, y);
  put_i16 (buf + 5, dx);
  put_i16 (buf + 7, dy);
  buf[9] = mods;
  return 10;
}

size_t
proto_encode_resize (uint8_t *buf, uint16_t pixel_w, uint16_t pixel_h)
{
  buf[0] = OP_RESIZE;
  put_u16 (buf + 1, pixel_w);
  put_u16 (buf + 3, pixel_h);
  return 5;
}

size_t
proto_encode_focus (uint8_t *buf, uint8_t gained)
{
  buf[0] = OP_FOCUS;
  buf[1] = gained;
  return 2;
}

size_t
proto_encode_interrupt (uint8_t *buf)
{
  buf[0] = OP_INTERRUPT;
  return 1;
}

size_t
proto_encode_font_metrics (uint8_t *buf, uint16_t char_w, uint16_t char_h,
                           uint16_t ascent, uint16_t descent)
{
  buf[0] = OP_FONT_METRICS;
  put_u16 (buf + 1, char_w);
  put_u16 (buf + 3, char_h);
  put_u16 (buf + 5, ascent);
  put_u16 (buf + 7, descent);
  return 9;
}


/* Return fixed size (including opcode byte) for known opcodes.
   For variable-length opcodes, return the fixed header size
   (caller must read additional variable data).
   Returns -1 for unknown opcodes.  */

int
proto_fixed_size (uint8_t opcode)
{
  switch (opcode)
    {
    case OP_CLEAR_RECT:    return 13;
    case OP_DRAW_GLYPHS:   return 16;   /* + len bytes of utf8 */
    case OP_DRAW_CURSOR:   return 14;
    case OP_SCROLL_RECT:   return 11;
    case OP_FILL_RECT:     return 13;
    case OP_FLUSH:         return 1;
    case OP_FRAME_SIZE:    return 11;
    case OP_DRAW_IMAGE:    return 13;   /* + len bytes of png */
    case OP_HEARTBEAT:     return 9;
    case OP_KEY_EVENT:     return 10;
    case OP_MOUSE_BUTTON:  return 8;
    case OP_MOUSE_MOVE:    return 6;
    case OP_SCROLL:        return 10;
    case OP_RESIZE:        return 5;
    case OP_FOCUS:         return 2;
    case OP_CLIPBOARD:     return 6;    /* + len bytes of utf8 */
    case OP_INTERRUPT:     return 1;
    case OP_FONT_METRICS:  return 9;
    default:               return -1;
    }
}


/* Decode one message from buf.  */

int
proto_decode (const uint8_t *buf, size_t len, struct proto_msg *msg)
{
  if (len < 1)
    return 0;

  uint8_t op = buf[0];
  int fixed = proto_fixed_size (op);
  if (fixed < 0)
    return -1;

  if ((size_t)fixed > len)
    return 0;

  msg->opcode = op;

  switch (op)
    {
    case OP_CLEAR_RECT:
      msg->d.clear_rect.x     = get_u16 (buf + 1);
      msg->d.clear_rect.y     = get_u16 (buf + 3);
      msg->d.clear_rect.w     = get_u16 (buf + 5);
      msg->d.clear_rect.h     = get_u16 (buf + 7);
      msg->d.clear_rect.color = get_u32 (buf + 9);
      return 13;

    case OP_DRAW_GLYPHS:
      {
        msg->d.draw_glyphs.x     = get_u16 (buf + 1);
        msg->d.draw_glyphs.y     = get_u16 (buf + 3);
        msg->d.draw_glyphs.fg    = get_u32 (buf + 5);
        msg->d.draw_glyphs.bg    = get_u32 (buf + 9);
        msg->d.draw_glyphs.flags = buf[13];
        uint16_t slen = get_u16 (buf + 14);
        msg->d.draw_glyphs.len   = slen;
        size_t total = 16 + (size_t)slen;
        if (total > len)
          return 0;
        msg->d.draw_glyphs.utf8 = buf + 16;
        return (int)total;
      }

    case OP_DRAW_CURSOR:
      msg->d.draw_cursor.x     = get_u16 (buf + 1);
      msg->d.draw_cursor.y     = get_u16 (buf + 3);
      msg->d.draw_cursor.w     = get_u16 (buf + 5);
      msg->d.draw_cursor.h     = get_u16 (buf + 7);
      msg->d.draw_cursor.type  = buf[9];
      msg->d.draw_cursor.color = get_u32 (buf + 10);
      return 14;

    case OP_SCROLL_RECT:
      msg->d.scroll_rect.x  = get_u16 (buf + 1);
      msg->d.scroll_rect.y  = get_u16 (buf + 3);
      msg->d.scroll_rect.w  = get_u16 (buf + 5);
      msg->d.scroll_rect.h  = get_u16 (buf + 7);
      msg->d.scroll_rect.dy = get_i16 (buf + 9);
      return 11;

    case OP_FILL_RECT:
      msg->d.fill_rect.x     = get_u16 (buf + 1);
      msg->d.fill_rect.y     = get_u16 (buf + 3);
      msg->d.fill_rect.w     = get_u16 (buf + 5);
      msg->d.fill_rect.h     = get_u16 (buf + 7);
      msg->d.fill_rect.color = get_u32 (buf + 9);
      return 13;

    case OP_FLUSH:
      return 1;

    case OP_FRAME_SIZE:
      msg->d.frame_size.frame_id = get_u16 (buf + 1);
      msg->d.frame_size.w        = get_u16 (buf + 3);
      msg->d.frame_size.h        = get_u16 (buf + 5);
      msg->d.frame_size.cw       = get_u16 (buf + 7);
      msg->d.frame_size.ch       = get_u16 (buf + 9);
      return 11;

    case OP_DRAW_IMAGE:
      {
        msg->d.draw_image.x = get_u16 (buf + 1);
        msg->d.draw_image.y = get_u16 (buf + 3);
        msg->d.draw_image.w = get_u16 (buf + 5);
        msg->d.draw_image.h = get_u16 (buf + 7);
        uint32_t ilen = get_u32 (buf + 9);
        msg->d.draw_image.len = ilen;
        size_t total = 13 + (size_t)ilen;
        if (total > len)
          return 0;
        msg->d.draw_image.png = buf + 13;
        return (int)total;
      }

    case OP_HEARTBEAT:
      msg->d.heartbeat.timestamp_ms = get_u64 (buf + 1);
      return 9;

    case OP_KEY_EVENT:
      msg->d.key_event.keycode    = get_u32 (buf + 1);
      msg->d.key_event.modifiers  = buf[5];
      msg->d.key_event.utf32_char = get_u32 (buf + 6);
      return 10;

    case OP_MOUSE_BUTTON:
      msg->d.mouse_button.x       = get_u16 (buf + 1);
      msg->d.mouse_button.y       = get_u16 (buf + 3);
      msg->d.mouse_button.button  = buf[5];
      msg->d.mouse_button.pressed = buf[6];
      msg->d.mouse_button.mods    = buf[7];
      return 8;

    case OP_MOUSE_MOVE:
      msg->d.mouse_move.x    = get_u16 (buf + 1);
      msg->d.mouse_move.y    = get_u16 (buf + 3);
      msg->d.mouse_move.mods = buf[5];
      return 6;

    case OP_SCROLL:
      msg->d.scroll.x    = get_u16 (buf + 1);
      msg->d.scroll.y    = get_u16 (buf + 3);
      msg->d.scroll.dx   = get_i16 (buf + 5);
      msg->d.scroll.dy   = get_i16 (buf + 7);
      msg->d.scroll.mods = buf[9];
      return 10;

    case OP_RESIZE:
      msg->d.resize.pixel_w = get_u16 (buf + 1);
      msg->d.resize.pixel_h = get_u16 (buf + 3);
      return 5;

    case OP_FOCUS:
      msg->d.focus.gained = buf[1];
      return 2;

    case OP_CLIPBOARD:
      {
        msg->d.clipboard.dir = buf[1];
        uint32_t clen = get_u32 (buf + 2);
        msg->d.clipboard.len = clen;
        size_t total = 6 + (size_t)clen;
        if (total > len)
          return 0;
        msg->d.clipboard.utf8 = buf + 6;
        return (int)total;
      }

    case OP_INTERRUPT:
      return 1;

    case OP_FONT_METRICS:
      msg->d.font_metrics.char_w  = get_u16 (buf + 1);
      msg->d.font_metrics.char_h  = get_u16 (buf + 3);
      msg->d.font_metrics.ascent  = get_u16 (buf + 5);
      msg->d.font_metrics.descent = get_u16 (buf + 7);
      return 9;

    default:
      return -1;
    }
}
