/* RFC 6455 WebSocket server for Emacs web display proxy.  */

#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include <stddef.h>

/* WebSocket opcodes.  */
#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

/* Maximum number of simultaneous clients.  */
#define WS_MAX_CLIENTS 32

/* Read buffer size per client.  */
#define WS_READ_BUF_SIZE (64 * 1024)

/* Client states.  */
enum ws_client_state
{
  WS_STATE_HTTP,        /* waiting for HTTP upgrade request */
  WS_STATE_OPEN,        /* WebSocket connection established */
  WS_STATE_CLOSING      /* close frame sent, waiting for response */
};

struct ws_client
{
  int fd;
  enum ws_client_state state;

  /* Read buffer for incoming data.  */
  uint8_t read_buf[WS_READ_BUF_SIZE];
  size_t read_len;
};

struct ws_server
{
  int listen_fd;
  int port;

  struct ws_client clients[WS_MAX_CLIENTS];
  int num_clients;
};

/* Create a WebSocket server listening on the given port.
   Returns 0 on success, -1 on error.  */
int ws_server_create (struct ws_server *srv, int port);

/* Accept a new client if one is pending.
   Returns the client index, or -1 if no client accepted.  */
int ws_server_accept (struct ws_server *srv);

/* Process incoming data on a client.  Call this when the client fd
   is readable.  Handles HTTP upgrade and WebSocket framing.
   callback is called for each complete binary message received.
   Returns 0 normally, -1 if client should be removed.  */
int ws_client_read (struct ws_server *srv, int client_idx,
                    void (*callback)(struct ws_server *srv,
                                     int client_idx,
                                     const uint8_t *data,
                                     size_t len,
                                     void *userdata),
                    void *userdata);

/* Send a binary WebSocket frame to a specific client.
   Returns 0 on success, -1 on error.  */
int ws_send_binary (struct ws_server *srv, int client_idx,
                    const uint8_t *data, size_t len);

/* Send a binary WebSocket frame to all connected clients.
   Returns number of clients successfully sent to.  */
int ws_broadcast_binary (struct ws_server *srv,
                         const uint8_t *data, size_t len);

/* Send a text WebSocket frame to a specific client.
   Returns 0 on success, -1 on error.  */
int ws_send_text (struct ws_server *srv, int client_idx,
                  const uint8_t *data, size_t len);

/* Send a text WebSocket frame to all connected clients.
   Returns number of clients successfully sent to.  */
int ws_broadcast_text (struct ws_server *srv,
                       const uint8_t *data, size_t len);

/* Remove a client (close fd, shift array).  */
void ws_remove_client (struct ws_server *srv, int client_idx);

/* Close all clients and the server socket.  */
void ws_server_destroy (struct ws_server *srv);

#endif /* WEBSOCKET_H */
