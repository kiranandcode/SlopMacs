/* Wire protocol for Emacs web display backend.
   Binary, little-endian.  All messages prefixed with u8 opcode.  */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Draw commands (Emacs -> Proxy -> Browser).  */
#define OP_CLEAR_RECT    0x01
#define OP_DRAW_GLYPHS   0x02
#define OP_DRAW_CURSOR   0x03
#define OP_SCROLL_RECT   0x04
#define OP_FILL_RECT     0x05
#define OP_FLUSH         0x06
#define OP_FRAME_SIZE    0x07
#define OP_DRAW_IMAGE    0x08
#define OP_HEARTBEAT     0x09

/* Input events (Browser -> Proxy -> Emacs).  */
#define OP_KEY_EVENT     0x80
#define OP_MOUSE_BUTTON  0x81
#define OP_MOUSE_MOVE    0x82
#define OP_SCROLL        0x83
#define OP_RESIZE        0x84
#define OP_FOCUS         0x85
#define OP_CLIPBOARD     0x86
#define OP_INTERRUPT     0x87
#define OP_FONT_METRICS  0xF1

/* DRAW_GLYPHS flags.  */
#define GLYPH_BOLD          (1 << 0)
#define GLYPH_ITALIC        (1 << 1)
#define GLYPH_UNDERLINE     (1 << 2)
#define GLYPH_STRIKETHROUGH (1 << 3)
#define GLYPH_BOX           (1 << 4)

/* Decoded message union.  */
struct proto_msg
{
  uint8_t opcode;
  union
  {
    struct { uint16_t x, y, w, h; uint32_t color; } clear_rect;
    struct { uint16_t x, y; uint32_t fg, bg; uint8_t flags;
             uint16_t len; const uint8_t *utf8; } draw_glyphs;
    struct { uint16_t x, y, w, h; uint8_t type; uint32_t color; } draw_cursor;
    struct { uint16_t x, y, w, h; int16_t dy; } scroll_rect;
    struct { uint16_t x, y, w, h; uint32_t color; } fill_rect;
    /* flush: no fields */
    struct { uint16_t frame_id, w, h, cw, ch; } frame_size;
    struct { uint16_t x, y, w, h; uint32_t len; const uint8_t *png; } draw_image;
    struct { uint64_t timestamp_ms; } heartbeat;
    struct { uint32_t keycode; uint8_t modifiers; uint32_t utf32_char; } key_event;
    struct { uint16_t x, y; uint8_t button, pressed, mods; } mouse_button;
    struct { uint16_t x, y; uint8_t mods; } mouse_move;
    struct { uint16_t x, y; int16_t dx, dy; uint8_t mods; } scroll;
    struct { uint16_t pixel_w, pixel_h; } resize;
    struct { uint8_t gained; } focus;
    struct { uint8_t dir; uint32_t len; const uint8_t *utf8; } clipboard;
    /* interrupt: no fields */
    struct { uint16_t char_w, char_h, ascent, descent; } font_metrics;
  } d;
};

/* Encode functions.  All return number of bytes written to buf.
   Caller must ensure buf has enough space.  */
size_t proto_encode_clear_rect (uint8_t *buf, uint16_t x, uint16_t y,
                                uint16_t w, uint16_t h, uint32_t color);
size_t proto_encode_draw_glyphs (uint8_t *buf, uint16_t x, uint16_t y,
                                 uint32_t fg, uint32_t bg, uint8_t flags,
                                 const uint8_t *utf8, uint16_t len);
size_t proto_encode_draw_cursor (uint8_t *buf, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h, uint8_t type,
                                 uint32_t color);
size_t proto_encode_scroll_rect (uint8_t *buf, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h, int16_t dy);
size_t proto_encode_fill_rect (uint8_t *buf, uint16_t x, uint16_t y,
                               uint16_t w, uint16_t h, uint32_t color);
size_t proto_encode_flush (uint8_t *buf);
size_t proto_encode_frame_size (uint8_t *buf, uint16_t frame_id,
                                uint16_t w, uint16_t h,
                                uint16_t cw, uint16_t ch);
size_t proto_encode_draw_image (uint8_t *buf, uint16_t x, uint16_t y,
                                uint16_t w, uint16_t h,
                                const uint8_t *png, uint32_t len);
size_t proto_encode_heartbeat (uint8_t *buf, uint64_t timestamp_ms);

size_t proto_encode_key_event (uint8_t *buf, uint32_t keycode,
                               uint8_t modifiers, uint32_t utf32_char);
size_t proto_encode_mouse_button (uint8_t *buf, uint16_t x, uint16_t y,
                                  uint8_t button, uint8_t pressed,
                                  uint8_t mods);
size_t proto_encode_mouse_move (uint8_t *buf, uint16_t x, uint16_t y,
                                uint8_t mods);
size_t proto_encode_scroll (uint8_t *buf, uint16_t x, uint16_t y,
                            int16_t dx, int16_t dy, uint8_t mods);
size_t proto_encode_resize (uint8_t *buf, uint16_t pixel_w, uint16_t pixel_h);
size_t proto_encode_focus (uint8_t *buf, uint8_t gained);
size_t proto_encode_interrupt (uint8_t *buf);
size_t proto_encode_font_metrics (uint8_t *buf, uint16_t char_w,
                                  uint16_t char_h, uint16_t ascent,
                                  uint16_t descent);

/* Decode one message from buf.  Returns bytes consumed, or 0 if
   buf is too short (need more data), or -1 on error.
   For variable-length messages (DRAW_GLYPHS, DRAW_IMAGE, CLIPBOARD),
   the pointers in msg->d point into buf — do not free buf while using msg.  */
int proto_decode (const uint8_t *buf, size_t len, struct proto_msg *msg);

/* Return the fixed-size portion of a message given its opcode,
   or -1 if unknown.  Does not include variable-length payload.  */
int proto_fixed_size (uint8_t opcode);

#endif /* PROTOCOL_H */
