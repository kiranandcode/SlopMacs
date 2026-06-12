/* Emacs web display proxy — JSON gateway.
   Receives NDJSON lines from Emacs and forwards as text WebSocket
   frames to browser clients.  Receives JSON from browsers and
   forwards to Emacs.

   Also runs a debug TCP server (port+1) for remote JS evaluation
   in the browser.  Connect with: nc localhost 8081
   Type JS expressions, get JSON results back.  */

#include "websocket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>

#ifdef USE_KQUEUE
#include <sys/event.h>
#endif
#ifdef USE_EPOLL
#include <sys/epoll.h>
#endif

/* Global state.  */
static struct ws_server server;
static int emacs_fd = -1;          /* fd to talk to Emacs (or stdin) */
static int emacs_listen_fd = -1;   /* listener for Emacs to (re)attach
                                      over TCP (--emacs-port).  In this
                                      mode the proxy outlives Emacs:
                                      hot reloads reconnect here and
                                      the browser keeps its WebSocket. */
static volatile sig_atomic_t running = 1;
static int active_input_fd = -1;   /* only forward input from this client fd */

/* Line buffer for data from Emacs.  */
#define EMACS_BUF_SIZE (2 * 1024 * 1024)
static uint8_t emacs_buf[EMACS_BUF_SIZE];
static size_t emacs_buf_len = 0;

/* Debug REPL state.  */
static int debug_listen_fd = -1;
static int debug_client_fd = -1;
static uint8_t debug_buf[4096];
static size_t debug_buf_len = 0;

static void
handle_signal (int sig)
{
  (void)sig;
  running = 0;
}

/* Create a TCP listener on the given port.  Returns fd or -1.  */
static int
create_listener (int port)
{
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  int opt = 1;
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

  struct sockaddr_in addr = { 0 };
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  addr.sin_port = htons (port);

  if (bind (fd, (struct sockaddr *)&addr, sizeof addr) < 0
      || listen (fd, 1) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

/* Called when a browser client sends us a text message (JSON input event).
   Forward it to Emacs, unless it's a debug eval_result.  */
static void
on_client_message (struct ws_server *srv, int client_idx,
                   const uint8_t *data, size_t len, void *userdata)
{
  (void)userdata;

  if (len < 1)
    return;

  /* Only forward input from the active (most recently connected) client.
     This prevents duplicate key events when multiple browser tabs
     are open.  Debug eval_result and interrupt are always allowed.  */
  int client_fd = (client_idx >= 0 && client_idx < srv->num_clients)
    ? srv->clients[client_idx].fd : -1;

  /* Check for interrupt message — always process.  In legacy
     --emacs-fd mode our parent IS Emacs, so deliver SIGINT directly.
     In --emacs-port mode the parent is the supervisor (Electron), so
     forward the interrupt as data; Emacs's I/O thread acts on it.  */
  if (len > 10 && memmem (data, len, "\"interrupt\"", 11)
      && emacs_listen_fd < 0)
    {
      pid_t ppid = getppid ();
      if (ppid > 1)
        kill (ppid, SIGINT);
      return;
    }

  /* Check for eval_result — forward to debug client, not Emacs.  */
  if (memmem (data, len, "\"eval_result\"", 13))
    {
      if (debug_client_fd >= 0)
        {
          write (debug_client_fd, data, len);
          write (debug_client_fd, "\n", 1);
        }
      return;
    }

  /* Drop input from non-active clients to prevent double events.  */
  if (client_fd != active_input_fd && active_input_fd >= 0)
    return;

  /* Forward to Emacs — ensure newline termination.  While no Emacs is
     attached in --emacs-port mode, drop input instead of spraying it
     into our stdout/log.  */
  if (emacs_listen_fd >= 0 && emacs_fd < 0)
    return;
  int out_fd = (emacs_fd >= 0) ? emacs_fd : STDOUT_FILENO;
  size_t written = 0;
  while (written < len)
    {
      ssize_t n = write (out_fd, data + written, len - written);
      if (n <= 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
          break;
        }
      written += n;
    }

  /* Ensure newline.  */
  if (len > 0 && data[len - 1] != '\n')
    {
      uint8_t nl = '\n';
      write (out_fd, &nl, 1);
    }
}

/* Process NDJSON lines from Emacs.  For each complete line, broadcast
   as a text WebSocket frame.  */
static void
process_emacs_data (void)
{
  size_t pos = 0;

  while (pos < emacs_buf_len)
    {
      /* Find newline.  */
      uint8_t *nl = memchr (emacs_buf + pos, '\n', emacs_buf_len - pos);
      if (!nl)
        break; /* incomplete line */

      size_t line_len = (nl - (emacs_buf + pos)) + 1; /* include \n */

      /* Broadcast this JSON line as a text frame (without the \n).  */
      ws_broadcast_text (&server, emacs_buf + pos, line_len - 1);

      pos += line_len;
    }

  /* Move remaining data to front of buffer.  */
  if (pos > 0)
    {
      emacs_buf_len -= pos;
      if (emacs_buf_len > 0)
        memmove (emacs_buf, emacs_buf + pos, emacs_buf_len);
    }
}

/* Process lines from debug client.  Each line is JS code to eval.
   Wrap in {"type":"eval","code":"..."} and broadcast to browsers.  */
static void
process_debug_data (void)
{
  size_t pos = 0;

  while (pos < debug_buf_len)
    {
      uint8_t *nl = memchr (debug_buf + pos, '\n', debug_buf_len - pos);
      if (!nl)
        break;

      size_t line_len = nl - (debug_buf + pos);
      if (line_len == 0)
        {
          pos = (nl - debug_buf) + 1;
          continue;
        }

      /* Build eval JSON.  Escape the code for JSON string.  */
      char msg[8192];
      int mi = 0;
      mi += snprintf (msg + mi, sizeof msg - mi,
                       "{\"type\":\"eval\",\"code\":\"");

      for (size_t i = 0; i < line_len && mi < (int)sizeof msg - 10; i++)
        {
          uint8_t c = debug_buf[pos + i];
          if (c == '"')
            { msg[mi++] = '\\'; msg[mi++] = '"'; }
          else if (c == '\\')
            { msg[mi++] = '\\'; msg[mi++] = '\\'; }
          else if (c == '\t')
            { msg[mi++] = '\\'; msg[mi++] = 't'; }
          else if (c < 0x20)
            mi += snprintf (msg + mi, sizeof msg - mi, "\\u%04x", c);
          else
            msg[mi++] = c;
        }

      mi += snprintf (msg + mi, sizeof msg - mi, "\"}");

      /* Broadcast to all browser clients.  */
      ws_broadcast_text (&server, (uint8_t *)msg, mi);

      pos = (nl - debug_buf) + 1;
    }

  /* Move remaining data to front.  */
  if (pos > 0)
    {
      debug_buf_len -= pos;
      if (debug_buf_len > 0)
        memmove (debug_buf, debug_buf + pos, debug_buf_len);
    }
}

/* On new client connect, ask Emacs for a full redraw.  */
static void
request_redraw (void)
{
  int out_fd = (emacs_fd >= 0) ? emacs_fd : STDOUT_FILENO;
  const char *msg = "{\"type\":\"request_redraw\"}\n";
  size_t len = strlen (msg);
  size_t written = 0;
  while (written < len)
    {
      ssize_t n = write (out_fd, msg + written, len - written);
      if (n <= 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
          break;
        }
      written += n;
    }
}


/* --- Event loop --- */

#ifdef USE_KQUEUE

static void
event_loop (void)
{
  int kq = kqueue ();
  if (kq < 0)
    {
      perror ("kqueue");
      return;
    }

  /* Register listen socket and emacs fd.  */
  struct kevent changes[4];
  int nchanges = 0;

  EV_SET (&changes[nchanges++], server.listen_fd, EVFILT_READ,
          EV_ADD | EV_ENABLE, 0, 0, NULL);

  int in_fd = -1;
  if (emacs_listen_fd >= 0)
    /* Emacs attaches (and re-attaches across hot reloads) via the
       listener; no input fd until it connects.  */
    EV_SET (&changes[nchanges++], emacs_listen_fd, EVFILT_READ,
            EV_ADD | EV_ENABLE, 0, 0, NULL);
  else
    {
      in_fd = (emacs_fd >= 0) ? emacs_fd : STDIN_FILENO;
      fcntl (in_fd, F_SETFL, fcntl (in_fd, F_GETFL) | O_NONBLOCK);
      EV_SET (&changes[nchanges++], in_fd, EVFILT_READ,
              EV_ADD | EV_ENABLE, 0, 0, NULL);
    }

  /* Register debug listener if available.  */
  if (debug_listen_fd >= 0)
    EV_SET (&changes[nchanges++], debug_listen_fd, EVFILT_READ,
            EV_ADD | EV_ENABLE, 0, 0, NULL);

  kevent (kq, changes, nchanges, NULL, 0, NULL);

  while (running)
    {
      struct kevent events[32];
      struct timespec timeout = { 1, 0 }; /* 1 second */
      int nev = kevent (kq, NULL, 0, events, 32, &timeout);

      if (nev < 0)
        {
          if (errno == EINTR)
            continue;
          perror ("kevent");
          break;
        }

      for (int i = 0; i < nev; i++)
        {
          int fd = (int)events[i].ident;

          if (fd == server.listen_fd)
            {
              int idx = ws_server_accept (&server);
              if (idx >= 0)
                {
                  /* Register new client fd.  */
                  struct kevent ev;
                  EV_SET (&ev, server.clients[idx].fd, EVFILT_READ,
                          EV_ADD | EV_ENABLE, 0, 0, NULL);
                  kevent (kq, &ev, 1, NULL, 0, NULL);
                  /* Newest client becomes the active input source.  */
                  active_input_fd = server.clients[idx].fd;
                }
            }
          else if (emacs_listen_fd >= 0 && fd == emacs_listen_fd)
            {
              /* Emacs (re)attaching over TCP.  Only one at a time;
                 a new connection replaces the old.  */
              int newfd = accept (emacs_listen_fd, NULL, NULL);
              if (newfd >= 0)
                {
                  struct kevent ev;
                  if (emacs_fd >= 0)
                    {
                      EV_SET (&ev, emacs_fd, EVFILT_READ, EV_DELETE,
                              0, 0, NULL);
                      kevent (kq, &ev, 1, NULL, 0, NULL);
                      close (emacs_fd);
                    }
                  emacs_fd = newfd;
                  in_fd = newfd;
                  emacs_buf_len = 0;
                  fcntl (emacs_fd, F_SETFL,
                         fcntl (emacs_fd, F_GETFL) | O_NONBLOCK);
                  EV_SET (&ev, emacs_fd, EVFILT_READ,
                          EV_ADD | EV_ENABLE, 0, 0, NULL);
                  kevent (kq, &ev, 1, NULL, 0, NULL);
                  fprintf (stderr, "Emacs attached\n");
                  /* Ask the (possibly fresh) Emacs for a full frame so
                     connected browser clients repaint immediately.  */
                  request_redraw ();
                }
            }
          else if (fd == debug_listen_fd)
            {
              /* Accept debug client (only one at a time).  */
              if (debug_client_fd >= 0)
                {
                  struct kevent ev;
                  EV_SET (&ev, debug_client_fd, EVFILT_READ,
                          EV_DELETE, 0, 0, NULL);
                  kevent (kq, &ev, 1, NULL, 0, NULL);
                  close (debug_client_fd);
                }
              debug_client_fd = accept (debug_listen_fd, NULL, NULL);
              if (debug_client_fd >= 0)
                {
                  fcntl (debug_client_fd, F_SETFL,
                         fcntl (debug_client_fd, F_GETFL) | O_NONBLOCK);
                  struct kevent ev;
                  EV_SET (&ev, debug_client_fd, EVFILT_READ,
                          EV_ADD | EV_ENABLE, 0, 0, NULL);
                  kevent (kq, &ev, 1, NULL, 0, NULL);
                  debug_buf_len = 0;
                  fprintf (stderr, "Debug client connected\n");
                }
            }
          else if (fd == debug_client_fd)
            {
              ssize_t n = read (debug_client_fd,
                                debug_buf + debug_buf_len,
                                sizeof debug_buf - debug_buf_len);
              if (n > 0)
                {
                  debug_buf_len += n;
                  process_debug_data ();
                }
              else if (n == 0)
                {
                  struct kevent ev;
                  EV_SET (&ev, debug_client_fd, EVFILT_READ,
                          EV_DELETE, 0, 0, NULL);
                  kevent (kq, &ev, 1, NULL, 0, NULL);
                  close (debug_client_fd);
                  debug_client_fd = -1;
                  debug_buf_len = 0;
                  fprintf (stderr, "Debug client disconnected\n");
                }
            }
          else if (in_fd >= 0 && fd == in_fd)
            {
              /* Data from Emacs.  */
              ssize_t n = read (in_fd, emacs_buf + emacs_buf_len,
                                EMACS_BUF_SIZE - emacs_buf_len);
              if (n > 0)
                {
                  emacs_buf_len += n;
                  process_emacs_data ();
                }
              else if (n == 0)
                {
                  if (emacs_listen_fd >= 0)
                    {
                      /* Emacs is restarting (hot reload); keep the
                         browser connected and await re-attachment.  */
                      struct kevent ev;
                      EV_SET (&ev, in_fd, EVFILT_READ, EV_DELETE,
                              0, 0, NULL);
                      kevent (kq, &ev, 1, NULL, 0, NULL);
                      close (in_fd);
                      emacs_fd = -1;
                      in_fd = -1;
                      emacs_buf_len = 0;
                      fprintf (stderr,
                               "Emacs detached; awaiting reconnect\n");
                    }
                  else
                    {
                      fprintf (stderr, "Emacs fd closed\n");
                      running = 0;
                    }
                }
            }
          else
            {
              /* Find which client this fd belongs to.  */
              for (int c = 0; c < server.num_clients; c++)
                {
                  if (server.clients[c].fd == fd)
                    {
                      enum ws_client_state prev_state = server.clients[c].state;
                      int rc = ws_client_read (&server, c,
                                               on_client_message, NULL);
                      if (rc < 0)
                        {
                          /* Deregister from kqueue before removing.  */
                          struct kevent ev;
                          EV_SET (&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                          kevent (kq, &ev, 1, NULL, 0, NULL);
                          if (fd == active_input_fd)
                            active_input_fd = -1;
                          ws_remove_client (&server, c);
                        }
                      else if (server.clients[c].state == WS_STATE_OPEN
                               && prev_state == WS_STATE_HTTP)
                        {
                          /* Client just completed handshake — this is
                             now the active input source.  */
                          active_input_fd = fd;
                          request_redraw ();
                        }
                      break;
                    }
                }
            }
        }
    }

  close (kq);
}

#elif defined(USE_EPOLL)

static void
event_loop (void)
{
  int epfd = epoll_create1 (0);
  if (epfd < 0)
    {
      perror ("epoll_create1");
      return;
    }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = server.listen_fd;
  epoll_ctl (epfd, EPOLL_CTL_ADD, server.listen_fd, &ev);

  int in_fd = -1;
  if (emacs_listen_fd >= 0)
    {
      /* Emacs attaches (and re-attaches across hot reloads) via the
         listener; no input fd until it connects.  */
      ev.events = EPOLLIN;
      ev.data.fd = emacs_listen_fd;
      epoll_ctl (epfd, EPOLL_CTL_ADD, emacs_listen_fd, &ev);
    }
  else
    {
      in_fd = (emacs_fd >= 0) ? emacs_fd : STDIN_FILENO;
      fcntl (in_fd, F_SETFL, fcntl (in_fd, F_GETFL) | O_NONBLOCK);
      ev.events = EPOLLIN;
      ev.data.fd = in_fd;
      epoll_ctl (epfd, EPOLL_CTL_ADD, in_fd, &ev);
    }

  /* Register debug listener if available.  */
  if (debug_listen_fd >= 0)
    {
      ev.events = EPOLLIN;
      ev.data.fd = debug_listen_fd;
      epoll_ctl (epfd, EPOLL_CTL_ADD, debug_listen_fd, &ev);
    }

  while (running)
    {
      struct epoll_event events[32];
      int nev = epoll_wait (epfd, events, 32, 1000);

      if (nev < 0)
        {
          if (errno == EINTR)
            continue;
          perror ("epoll_wait");
          break;
        }

      for (int i = 0; i < nev; i++)
        {
          int fd = events[i].data.fd;

          if (fd == server.listen_fd)
            {
              int idx = ws_server_accept (&server);
              if (idx >= 0)
                {
                  struct epoll_event cev;
                  cev.events = EPOLLIN;
                  cev.data.fd = server.clients[idx].fd;
                  epoll_ctl (epfd, EPOLL_CTL_ADD,
                             server.clients[idx].fd, &cev);
                  active_input_fd = server.clients[idx].fd;
                }
            }
          else if (emacs_listen_fd >= 0 && fd == emacs_listen_fd)
            {
              /* Emacs (re)attaching over TCP.  */
              int newfd = accept (emacs_listen_fd, NULL, NULL);
              if (newfd >= 0)
                {
                  struct epoll_event eev;
                  if (emacs_fd >= 0)
                    {
                      epoll_ctl (epfd, EPOLL_CTL_DEL, emacs_fd, NULL);
                      close (emacs_fd);
                    }
                  emacs_fd = newfd;
                  in_fd = newfd;
                  emacs_buf_len = 0;
                  fcntl (emacs_fd, F_SETFL,
                         fcntl (emacs_fd, F_GETFL) | O_NONBLOCK);
                  eev.events = EPOLLIN;
                  eev.data.fd = emacs_fd;
                  epoll_ctl (epfd, EPOLL_CTL_ADD, emacs_fd, &eev);
                  fprintf (stderr, "Emacs attached\n");
                  request_redraw ();
                }
            }
          else if (fd == debug_listen_fd)
            {
              if (debug_client_fd >= 0)
                {
                  epoll_ctl (epfd, EPOLL_CTL_DEL, debug_client_fd, NULL);
                  close (debug_client_fd);
                }
              debug_client_fd = accept (debug_listen_fd, NULL, NULL);
              if (debug_client_fd >= 0)
                {
                  fcntl (debug_client_fd, F_SETFL,
                         fcntl (debug_client_fd, F_GETFL) | O_NONBLOCK);
                  struct epoll_event cev;
                  cev.events = EPOLLIN;
                  cev.data.fd = debug_client_fd;
                  epoll_ctl (epfd, EPOLL_CTL_ADD, debug_client_fd, &cev);
                  debug_buf_len = 0;
                  fprintf (stderr, "Debug client connected\n");
                }
            }
          else if (fd == debug_client_fd)
            {
              ssize_t n = read (debug_client_fd,
                                debug_buf + debug_buf_len,
                                sizeof debug_buf - debug_buf_len);
              if (n > 0)
                {
                  debug_buf_len += n;
                  process_debug_data ();
                }
              else if (n == 0)
                {
                  epoll_ctl (epfd, EPOLL_CTL_DEL, debug_client_fd, NULL);
                  close (debug_client_fd);
                  debug_client_fd = -1;
                  debug_buf_len = 0;
                  fprintf (stderr, "Debug client disconnected\n");
                }
            }
          else if (in_fd >= 0 && fd == in_fd)
            {
              ssize_t n = read (in_fd, emacs_buf + emacs_buf_len,
                                EMACS_BUF_SIZE - emacs_buf_len);
              if (n > 0)
                {
                  emacs_buf_len += n;
                  process_emacs_data ();
                }
              else if (n == 0)
                {
                  if (emacs_listen_fd >= 0)
                    {
                      /* Emacs is restarting (hot reload); keep the
                         browser connected and await re-attachment.  */
                      epoll_ctl (epfd, EPOLL_CTL_DEL, in_fd, NULL);
                      close (in_fd);
                      emacs_fd = -1;
                      in_fd = -1;
                      emacs_buf_len = 0;
                      fprintf (stderr,
                               "Emacs detached; awaiting reconnect\n");
                    }
                  else
                    {
                      fprintf (stderr, "Emacs fd closed\n");
                      running = 0;
                    }
                }
            }
          else
            {
              for (int c = 0; c < server.num_clients; c++)
                {
                  if (server.clients[c].fd == fd)
                    {
                      enum ws_client_state prev_state = server.clients[c].state;
                      int rc = ws_client_read (&server, c,
                                               on_client_message, NULL);
                      if (rc < 0)
                        {
                          epoll_ctl (epfd, EPOLL_CTL_DEL, fd, NULL);
                          if (fd == active_input_fd)
                            active_input_fd = -1;
                          ws_remove_client (&server, c);
                        }
                      else if (server.clients[c].state == WS_STATE_OPEN
                               && prev_state == WS_STATE_HTTP)
                        {
                          active_input_fd = fd;
                          request_redraw ();
                        }
                      break;
                    }
                }
            }
        }
    }

  close (epfd);
}

#else
#error "Neither USE_KQUEUE nor USE_EPOLL defined"
#endif


static void
usage (const char *prog)
{
  fprintf (stderr,
           "Usage: %s [OPTIONS]\n"
           "  --port PORT         WebSocket port (default: 8080)\n"
           "  --emacs-fd FD       File descriptor to communicate with Emacs\n"
           "  --emacs-port PORT   Listen for Emacs to (re)attach over TCP;\n"
           "                      the proxy then survives Emacs restarts\n"
           "  --help              Show this help\n",
           prog);
}

int
main (int argc, char **argv)
{
  int port = 8080;

  for (int i = 1; i < argc; i++)
    {
      int emacs_port = 0;
      if (strcmp (argv[i], "--port") == 0 && i + 1 < argc)
        port = atoi (argv[++i]);
      else if (strcmp (argv[i], "--emacs-fd") == 0 && i + 1 < argc)
        emacs_fd = atoi (argv[++i]);
      else if (strcmp (argv[i], "--emacs-port") == 0 && i + 1 < argc
               && (emacs_port = atoi (argv[++i])) > 0)
        {
          emacs_listen_fd = create_listener (emacs_port);
          if (emacs_listen_fd < 0)
            {
              fprintf (stderr, "Cannot listen on emacs port %d\n", emacs_port);
              return 1;
            }
          fprintf (stderr, "Awaiting Emacs on port %d\n", emacs_port);
        }
      else if (strcmp (argv[i], "--help") == 0)
        {
          usage (argv[0]);
          return 0;
        }
      else
        {
          fprintf (stderr, "Unknown option: %s\n", argv[i]);
          usage (argv[0]);
          return 1;
        }
    }

  signal (SIGINT, handle_signal);
  signal (SIGTERM, handle_signal);
  signal (SIGPIPE, SIG_IGN);

  if (ws_server_create (&server, port) < 0)
    return 1;

  /* Start debug REPL listener on port+1.  */
  debug_listen_fd = create_listener (port + 1);
  if (debug_listen_fd >= 0)
    fprintf (stderr, "Debug REPL on port %d (nc localhost %d)\n",
             port + 1, port + 1);

  fprintf (stderr, "emacs-web-display started (port=%d emacs_fd=%d)\n",
           port, emacs_fd);

  event_loop ();

  if (debug_client_fd >= 0)
    close (debug_client_fd);
  if (debug_listen_fd >= 0)
    close (debug_listen_fd);

  ws_server_destroy (&server);
  fprintf (stderr, "emacs-web-display exiting\n");
  return 0;
}
