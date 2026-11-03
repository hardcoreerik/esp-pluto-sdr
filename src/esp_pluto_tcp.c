#include "esp_pluto_tcp.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif
static int tcp_read(void *p, void *b, size_t n) {
  pluto_tcp *t = p;
  if (n > INT_MAX)
    n = INT_MAX;
  int r = (int)recv(t->fd, b, n, 0);
  return r > 0 ? r : PLUTO_IO;
}
static int tcp_write(void *p, const void *b, size_t n) {
  pluto_tcp *t = p;
  if (n > INT_MAX)
    n = INT_MAX;
#ifdef MSG_NOSIGNAL
  int flags = MSG_NOSIGNAL;
#else
  int flags = 0;
#endif
  int r = (int)send(t->fd, b, n, flags);
  return r > 0 ? r : PLUTO_IO;
}
void pluto_tcp_disconnect(pluto_tcp *t) {
  if (t && t->fd >= 0) {
    shutdown(t->fd, SHUT_RDWR);
    close(t->fd);
    t->fd = -1;
  }
}
int pluto_tcp_connect(pluto_tcp *t, const char *ip, uint16_t port, unsigned ms,
                      pluto_io *out) {
  if (!t || !ip || !port || !ms || ms > 60000 || !out)
    return PLUTO_INVALID;
  t->fd = -1;
  memset(out, 0, sizeof *out);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1)
    return PLUTO_INVALID;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return PLUTO_IO;
  t->fd = fd;
  int old = fcntl(fd, F_GETFL, 0);
  if (old < 0 || fcntl(fd, F_SETFL, old | O_NONBLOCK) < 0)
    goto fail;
  int r = connect(fd, (struct sockaddr *)&a, sizeof a);
  if (r < 0) {
    if (errno != EINPROGRESS)
      goto fail;
    if (fd >= FD_SETSIZE)
      goto fail;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
    r = select(fd + 1, NULL, &set, NULL, &tv);
    if (r <= 0)
      goto fail;
    int error = 0;
    socklen_t len = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error)
      goto fail;
  }
  if (fcntl(fd, F_SETFL, old) < 0)
    goto fail;
  struct timeval tv = {(long)(ms / 1000), (long)(ms % 1000) * 1000};
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
    goto fail;
  *out = (pluto_io){t, tcp_read, tcp_write};
  return 0;
fail:
  pluto_tcp_disconnect(t);
  return PLUTO_IO;
}
