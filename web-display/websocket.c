/* RFC 6455 WebSocket server for Emacs web display proxy.  */

#include "websocket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* We implement SHA-1 inline to avoid external dependencies.
   Only used for the WebSocket handshake.  */

/* --- Minimal SHA-1 implementation --- */

struct sha1_ctx
{
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
};

static void
sha1_init (struct sha1_ctx *ctx)
{
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count = 0;
}

static uint32_t
rotl32 (uint32_t x, int n)
{
  return (x << n) | (x >> (32 - n));
}

static void
sha1_transform (uint32_t state[5], const uint8_t block[64])
{
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16)
          | ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
  for (int i = 16; i < 80; i++)
    w[i] = rotl32 (w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

  uint32_t a = state[0], b = state[1], c = state[2],
           d = state[3], e = state[4];

  for (int i = 0; i < 80; i++)
    {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else              { f = b ^ c ^ d;             k = 0xCA62C1D6; }

      uint32_t tmp = rotl32 (a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl32 (b, 30); b = a; a = tmp;
    }

  state[0] += a; state[1] += b; state[2] += c;
  state[3] += d; state[4] += e;
}

static void
sha1_update (struct sha1_ctx *ctx, const uint8_t *data, size_t len)
{
  size_t idx = (size_t)(ctx->count & 63);
  ctx->count += len;

  for (size_t i = 0; i < len; i++)
    {
      ctx->buffer[idx++] = data[i];
      if (idx == 64)
        {
          sha1_transform (ctx->state, ctx->buffer);
          idx = 0;
        }
    }
}

static void
sha1_final (struct sha1_ctx *ctx, uint8_t digest[20])
{
  uint64_t bits = ctx->count * 8;
  uint8_t pad = 0x80;
  sha1_update (ctx, &pad, 1);
  pad = 0;
  while ((ctx->count & 63) != 56)
    sha1_update (ctx, &pad, 1);

  uint8_t bits_be[8];
  for (int i = 0; i < 8; i++)
    bits_be[i] = (uint8_t)(bits >> (56 - i * 8));
  sha1_update (ctx, bits_be, 8);

  for (int i = 0; i < 5; i++)
    {
      digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
      digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
      digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
      digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* --- Base64 encode --- */

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t
base64_encode (const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
  size_t olen = 4 * ((in_len + 2) / 3);
  if (olen + 1 > out_size)
    return 0;

  size_t j = 0;
  for (size_t i = 0; i < in_len; i += 3)
    {
      uint32_t a = in[i];
      uint32_t b = (i + 1 < in_len) ? in[i+1] : 0;
      uint32_t c = (i + 2 < in_len) ? in[i+2] : 0;
      uint32_t triple = (a << 16) | (b << 8) | c;

      out[j++] = b64_table[(triple >> 18) & 0x3F];
      out[j++] = b64_table[(triple >> 12) & 0x3F];
      out[j++] = (i + 1 < in_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
      out[j++] = (i + 2 < in_len) ? b64_table[triple & 0x3F] : '=';
    }
  out[j] = '\0';
  return j;
}

/* --- WebSocket handshake --- */

static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* Extract Sec-WebSocket-Key from HTTP request headers.
   Returns pointer into buf, or NULL.  */
static const char *
find_ws_key (const char *buf)
{
  const char *p = strstr (buf, "Sec-WebSocket-Key:");
  if (!p)
    p = strstr (buf, "sec-websocket-key:");
  if (!p)
    return NULL;
  p += 18; /* skip header name + colon */
  while (*p == ' ')
    p++;
  return p;
}

/* Perform the HTTP upgrade handshake for a client.
   buf contains the HTTP request.  Returns 0 on success, -1 on error.  */
static int
ws_do_handshake (int fd, const char *buf)
{
  const char *key = find_ws_key (buf);
  if (!key)
    return -1;

  /* Extract the key value (up to \r\n).  */
  char key_buf[64];
  int klen = 0;
  while (key[klen] && key[klen] != '\r' && key[klen] != '\n'
         && klen < 63)
    {
      key_buf[klen] = key[klen];
      klen++;
    }
  key_buf[klen] = '\0';

  /* Trim trailing whitespace.  */
  while (klen > 0 && (key_buf[klen-1] == ' ' || key_buf[klen-1] == '\t'))
    key_buf[--klen] = '\0';

  /* Concatenate with magic string.  */
  char concat[128];
  snprintf (concat, sizeof concat, "%s%s", key_buf, ws_magic);

  /* SHA-1 hash.  */
  struct sha1_ctx sha;
  uint8_t hash[20];
  sha1_init (&sha);
  sha1_update (&sha, (const uint8_t *)concat, strlen (concat));
  sha1_final (&sha, hash);

  /* Base64 encode.  */
  char accept[32];
  base64_encode (hash, 20, accept, sizeof accept);

  /* Send response.  */
  char response[512];
  int rlen = snprintf (response, sizeof response,
                       "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: %s\r\n"
                       "\r\n",
                       accept);

  /* Write entire response (blocking is fine for handshake).  */
  ssize_t written = 0;
  while (written < rlen)
    {
      ssize_t n = write (fd, response + written, rlen - written);
      if (n <= 0)
        return -1;
      written += n;
    }

  return 0;
}

/* --- Non-blocking I/O helpers --- */

static int
set_nonblocking (int fd)
{
  int flags = fcntl (fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

static int
set_nodelay (int fd)
{
  int flag = 1;
  return setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
}

/* --- Server API --- */

int
ws_server_create (struct ws_server *srv, int port)
{
  memset (srv, 0, sizeof *srv);
  srv->port = port;
  srv->listen_fd = -1;

  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      perror ("socket");
      return -1;
    }

  int opt = 1;
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

  struct sockaddr_in addr;
  memset (&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_ANY);
  addr.sin_port = htons (port);

  if (bind (fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
      perror ("bind");
      close (fd);
      return -1;
    }

  if (listen (fd, 4) < 0)
    {
      perror ("listen");
      close (fd);
      return -1;
    }

  set_nonblocking (fd);
  srv->listen_fd = fd;
  fprintf (stderr, "WebSocket server listening on port %d\n", port);
  return 0;
}

int
ws_server_accept (struct ws_server *srv)
{
  if (srv->num_clients >= WS_MAX_CLIENTS)
    {
      /* Evict oldest client to make room.  */
      fprintf (stderr, "Max clients reached, evicting client 0\n");
      ws_remove_client (srv, 0);
    }

  struct sockaddr_in addr;
  socklen_t addrlen = sizeof addr;
  int fd = accept (srv->listen_fd, (struct sockaddr *)&addr, &addrlen);
  if (fd < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return -1;
      perror ("accept");
      return -1;
    }

  set_nonblocking (fd);
  set_nodelay (fd);

  int idx = srv->num_clients;
  struct ws_client *c = &srv->clients[idx];
  c->fd = fd;
  c->state = WS_STATE_HTTP;
  c->read_len = 0;
  srv->num_clients++;

  fprintf (stderr, "Client %d connected from %s:%d\n",
           idx, inet_ntoa (addr.sin_addr), ntohs (addr.sin_port));
  return idx;
}

/* Parse and process WebSocket frames from the client read buffer.
   Returns 0 to keep client, -1 to remove.  */
static int
ws_process_frames (struct ws_server *srv, int client_idx,
                   void (*callback)(struct ws_server *, int,
                                    const uint8_t *, size_t, void *),
                   void *userdata)
{
  struct ws_client *c = &srv->clients[client_idx];

  while (c->read_len >= 2)
    {
      uint8_t *buf = c->read_buf;
      int fin = (buf[0] >> 7) & 1;
      int opcode = buf[0] & 0x0F;
      int masked = (buf[1] >> 7) & 1;
      uint64_t payload_len = buf[1] & 0x7F;
      size_t header_len = 2;

      if (payload_len == 126)
        {
          if (c->read_len < 4) return 0;
          payload_len = ((uint64_t)buf[2] << 8) | buf[3];
          header_len = 4;
        }
      else if (payload_len == 127)
        {
          if (c->read_len < 10) return 0;
          payload_len = 0;
          for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | buf[2 + i];
          header_len = 10;
        }

      if (masked)
        header_len += 4;

      size_t total = header_len + (size_t)payload_len;
      if (total > c->read_len)
        return 0; /* need more data */

      /* Unmask payload if needed (client->server is always masked).  */
      uint8_t *payload = buf + header_len;
      if (masked)
        {
          uint8_t *mask = buf + header_len - 4;
          for (size_t i = 0; i < (size_t)payload_len; i++)
            payload[i] ^= mask[i & 3];
        }

      (void)fin; /* TODO: handle fragmented messages if needed */

      switch (opcode)
        {
        case WS_OP_BINARY:
          if (callback)
            callback (srv, client_idx, payload, (size_t)payload_len, userdata);
          break;

        case WS_OP_TEXT:
          /* Treat text same as binary for now.  */
          if (callback)
            callback (srv, client_idx, payload, (size_t)payload_len, userdata);
          break;

        case WS_OP_PING:
          {
            /* Reply with pong.  */
            uint8_t pong_hdr[10];
            size_t hlen;
            pong_hdr[0] = 0x80 | WS_OP_PONG;
            if (payload_len < 126)
              {
                pong_hdr[1] = (uint8_t)payload_len;
                hlen = 2;
              }
            else
              {
                pong_hdr[1] = 126;
                pong_hdr[2] = (uint8_t)(payload_len >> 8);
                pong_hdr[3] = (uint8_t)(payload_len);
                hlen = 4;
              }
            write (c->fd, pong_hdr, hlen);
            if (payload_len > 0)
              write (c->fd, payload, (size_t)payload_len);
          }
          break;

        case WS_OP_PONG:
          /* Ignore pong.  */
          break;

        case WS_OP_CLOSE:
          /* Send close frame back and mark for removal.  */
          {
            uint8_t close_frame[2] = { 0x80 | WS_OP_CLOSE, 0 };
            write (c->fd, close_frame, 2);
          }
          /* Consume frame then signal removal.  */
          memmove (buf, buf + total, c->read_len - total);
          c->read_len -= total;
          return -1;

        default:
          break;
        }

      /* Consume this frame.  */
      memmove (buf, buf + total, c->read_len - total);
      c->read_len -= total;
    }

  return 0;
}

int
ws_client_read (struct ws_server *srv, int client_idx,
                void (*callback)(struct ws_server *, int,
                                 const uint8_t *, size_t, void *),
                void *userdata)
{
  struct ws_client *c = &srv->clients[client_idx];

  /* Read available data.  */
  size_t space = WS_READ_BUF_SIZE - c->read_len;
  if (space == 0)
    {
      fprintf (stderr, "Client %d read buffer full\n", client_idx);
      return -1;
    }

  ssize_t n = read (c->fd, c->read_buf + c->read_len, space);
  if (n <= 0)
    {
      if (n == 0)
        fprintf (stderr, "Client %d disconnected\n", client_idx);
      else if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
          perror ("client read");
          return -1;
        }
      if (n == 0)
        return -1;
      return 0;
    }
  c->read_len += n;

  if (c->state == WS_STATE_HTTP)
    {
      /* Look for end of HTTP headers.  */
      if (c->read_len >= 4
          && memmem (c->read_buf, c->read_len, "\r\n\r\n", 4))
        {
          /* Null-terminate for string search (safe since we have space).  */
          if (c->read_len < WS_READ_BUF_SIZE)
            c->read_buf[c->read_len] = '\0';

          if (ws_do_handshake (c->fd, (const char *)c->read_buf) < 0)
            {
              fprintf (stderr, "Client %d handshake failed\n", client_idx);
              return -1;
            }

          /* Find end of HTTP request.  */
          uint8_t *end = (uint8_t *)memmem (c->read_buf, c->read_len,
                                            "\r\n\r\n", 4);
          size_t http_len = (end - c->read_buf) + 4;
          memmove (c->read_buf, c->read_buf + http_len,
                   c->read_len - http_len);
          c->read_len -= http_len;
          c->state = WS_STATE_OPEN;

          fprintf (stderr, "Client %d upgraded to WebSocket\n", client_idx);
        }
      return 0;
    }

  /* Process WebSocket frames.  */
  return ws_process_frames (srv, client_idx, callback, userdata);
}

int
ws_send_binary (struct ws_server *srv, int client_idx,
                const uint8_t *data, size_t len)
{
  if (client_idx < 0 || client_idx >= srv->num_clients)
    return -1;

  struct ws_client *c = &srv->clients[client_idx];
  if (c->state != WS_STATE_OPEN)
    return -1;

  /* Build WebSocket frame header (server->client: unmasked).  */
  uint8_t hdr[10];
  size_t hlen;
  hdr[0] = 0x80 | WS_OP_BINARY; /* FIN + binary */

  if (len < 126)
    {
      hdr[1] = (uint8_t)len;
      hlen = 2;
    }
  else if (len < 65536)
    {
      hdr[1] = 126;
      hdr[2] = (uint8_t)(len >> 8);
      hdr[3] = (uint8_t)(len);
      hlen = 4;
    }
  else
    {
      hdr[1] = 127;
      for (int i = 0; i < 8; i++)
        hdr[2 + i] = (uint8_t)(len >> (56 - i * 8));
      hlen = 10;
    }

  /* Write header + payload.  We do two writes; TCP_NODELAY ensures
     they go out promptly.  TODO: use writev for atomicity.  */
  ssize_t n = write (c->fd, hdr, hlen);
  if (n < (ssize_t)hlen)
    return -1;

  if (len > 0)
    {
      size_t written = 0;
      while (written < len)
        {
          n = write (c->fd, data + written, len - written);
          if (n <= 0)
            {
              if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* spin — acceptable for now */
              return -1;
            }
          written += n;
        }
    }

  return 0;
}

int
ws_broadcast_binary (struct ws_server *srv, const uint8_t *data, size_t len)
{
  int sent = 0;
  for (int i = 0; i < srv->num_clients; i++)
    {
      if (srv->clients[i].state == WS_STATE_OPEN)
        {
          if (ws_send_binary (srv, i, data, len) == 0)
            sent++;
        }
    }
  return sent;
}

int
ws_send_text (struct ws_server *srv, int client_idx,
              const uint8_t *data, size_t len)
{
  if (client_idx < 0 || client_idx >= srv->num_clients)
    return -1;

  struct ws_client *c = &srv->clients[client_idx];
  if (c->state != WS_STATE_OPEN)
    return -1;

  /* Build WebSocket frame header (server->client: unmasked).  */
  uint8_t hdr[10];
  size_t hlen;
  hdr[0] = 0x80 | WS_OP_TEXT; /* FIN + text */

  if (len < 126)
    {
      hdr[1] = (uint8_t)len;
      hlen = 2;
    }
  else if (len < 65536)
    {
      hdr[1] = 126;
      hdr[2] = (uint8_t)(len >> 8);
      hdr[3] = (uint8_t)(len);
      hlen = 4;
    }
  else
    {
      hdr[1] = 127;
      for (int i = 0; i < 8; i++)
        hdr[2 + i] = (uint8_t)(len >> (56 - i * 8));
      hlen = 10;
    }

  ssize_t n = write (c->fd, hdr, hlen);
  if (n < (ssize_t)hlen)
    return -1;

  if (len > 0)
    {
      size_t written = 0;
      while (written < len)
        {
          n = write (c->fd, data + written, len - written);
          if (n <= 0)
            {
              if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
              return -1;
            }
          written += n;
        }
    }

  return 0;
}

int
ws_broadcast_text (struct ws_server *srv, const uint8_t *data, size_t len)
{
  int sent = 0;
  for (int i = 0; i < srv->num_clients; i++)
    {
      if (srv->clients[i].state == WS_STATE_OPEN)
        {
          if (ws_send_text (srv, i, data, len) == 0)
            sent++;
        }
    }
  return sent;
}

void
ws_remove_client (struct ws_server *srv, int client_idx)
{
  if (client_idx < 0 || client_idx >= srv->num_clients)
    return;

  close (srv->clients[client_idx].fd);
  fprintf (stderr, "Client %d removed\n", client_idx);

  /* Shift remaining clients down.  */
  for (int i = client_idx; i < srv->num_clients - 1; i++)
    srv->clients[i] = srv->clients[i + 1];
  srv->num_clients--;
}

void
ws_server_destroy (struct ws_server *srv)
{
  for (int i = srv->num_clients - 1; i >= 0; i--)
    ws_remove_client (srv, i);

  if (srv->listen_fd >= 0)
    {
      close (srv->listen_fd);
      srv->listen_fd = -1;
    }
}
