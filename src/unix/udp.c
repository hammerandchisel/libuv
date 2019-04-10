/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__MVS__)
#include <xti.h>
#endif
#include <sys/un.h>

#if defined(IPV6_JOIN_GROUP) && !defined(IPV6_ADD_MEMBERSHIP)
# define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#endif

#if defined(IPV6_LEAVE_GROUP) && !defined(IPV6_DROP_MEMBERSHIP)
# define IPV6_DROP_MEMBERSHIP IPV6_LEAVE_GROUP
#endif


static void uv__udp_run_completed(uv_udp_t* handle);
static void uv__udp_io(uv_loop_t* loop, uv__io_t* w, unsigned int revents);
static void uv__udp_recvmsg(uv_udp_t* handle);
static void uv__udp_sendmsg(uv_udp_t* handle);
static int uv__udp_maybe_deferred_bind(uv_udp_t* handle,
                                       int domain,
                                       unsigned int flags);

#if defined(DISCORD_ENABLE_RECVMMSG)
#define DISCORD_RECVMMSG_BATCHSIZE 32
static void uv__udp_recvmmsg(uv_udp_t* handle);
#endif
#if defined(DISCORD_ENABLE_SENDMMSG)
#define DISCORD_SENDMMSG_BATCHSIZE 64
static void uv__udp_sendmmsg(uv_udp_t* handle);
#endif // DISCORD_ENABLE_SENDMMSG

#if defined(DISCORD_ENABLE_NETMAP)
void uv__udp_netmap_close_handle(uv_udp_t* handle);
void uv__udp_netmap_finish_close_handle(uv_udp_t* handle);
int uv__udp_netmap_bind(uv_udp_t* handle,
                        const struct sockaddr* addr,
                        unsigned int addrlen,
                        unsigned int flags);
int uv__udp_netmap_connect(uv_udp_t* handle,
                           const struct sockaddr* addr,
                           unsigned int addrlen);
int uv__udp_netmap_disconnect(uv_udp_t* handle);
int uv__udp_netmap_send(uv_udp_send_t* req,
                        uv_udp_t* handle,
                        const uv_buf_t bufs[],
                        unsigned int nbufs,
                        const struct sockaddr* addr,
                        unsigned int addrlen,
                        uv_udp_send_cb send_cb);
int uv__udp_netmap_try_send(uv_udp_t* handle,
                            const uv_buf_t bufs[],
                            unsigned int nbufs,
                            const struct sockaddr* addr,
                            unsigned int addrlen);
int uv__udp_netmap_set_membership(uv_udp_t* handle,
                                  const char* multicast_addr,
                                  const char* interface_addr,
                                  uv_membership membership);
int uv__udp_netmap_init_handle(uv_loop_t* loop, uv_udp_t* handle, unsigned int flags);
int uv__udp_netmap_open(uv_udp_t* handle, uv_os_sock_t sock);
int uv__udp_netmap_setsockopt(uv_udp_t* handle,
                              int option4,
                              int option6,
                              const void* val,
                              size_t size);
int uv__udp_netmap_set_broadcast(uv_udp_t* handle, int on);
int uv__udp_netmap_set_multicast_ttl(uv_udp_t* handle, int ttl);
int uv__udp_netmap_set_multicast_loop(uv_udp_t* handle, int on);
int uv__udp_netmap_set_multicast_interface(uv_udp_t* handle, const char* interface_addr);
int uv__udp_netmap_getpeername(const uv_udp_t* handle, struct sockaddr* name, int* namelen);
int uv__udp_netmap_getsockname(const uv_udp_t* handle, struct sockaddr* name, int* namelen);
int uv__udp_netmap_recv_start(uv_udp_t* handle, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb);
int uv__udp_netmap_recv_stop(uv_udp_t* handle);
#endif // DISCORD_ENABLE_NETMAP


void uv__udp_close(uv_udp_t* handle) {
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    uv__udp_netmap_close_handle(handle);
    return;
  }
#endif

  uv__io_close(handle->loop, &handle->io_watcher);
  uv__handle_stop(handle);

  if (handle->io_watcher.fd != -1) {
    uv__close(handle->io_watcher.fd);
    handle->io_watcher.fd = -1;
  }
}


void uv__udp_finish_close(uv_udp_t* handle) {
  uv_udp_send_t* req;
  QUEUE* q;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
     uv__udp_netmap_finish_close_handle(handle);
     return;
  }
#endif

  assert(!uv__io_active(&handle->io_watcher, POLLIN | POLLOUT));
  assert(handle->io_watcher.fd == -1);

  while (!QUEUE_EMPTY(&handle->write_queue)) {
    q = QUEUE_HEAD(&handle->write_queue);
    QUEUE_REMOVE(q);

    req = QUEUE_DATA(q, uv_udp_send_t, queue);
    req->status = UV_ECANCELED;
    QUEUE_INSERT_TAIL(&handle->write_completed_queue, &req->queue);
  }

  uv__udp_run_completed(handle);

  assert(handle->send_queue_size == 0);
  assert(handle->send_queue_count == 0);

  /* Now tear down the handle. */
  handle->recv_cb = NULL;
  handle->alloc_cb = NULL;
  /* but _do not_ touch close_cb */
}


static void uv__udp_run_completed(uv_udp_t* handle) {
  uv_udp_send_t* req;
  QUEUE* q;

  assert(!(handle->flags & UV_HANDLE_UDP_PROCESSING));
  handle->flags |= UV_HANDLE_UDP_PROCESSING;

  while (!QUEUE_EMPTY(&handle->write_completed_queue)) {
    q = QUEUE_HEAD(&handle->write_completed_queue);
    QUEUE_REMOVE(q);

    req = QUEUE_DATA(q, uv_udp_send_t, queue);
    uv__req_unregister(handle->loop, req);

    handle->send_queue_size -= uv__count_bufs(req->bufs, req->nbufs);
    handle->send_queue_count--;

    if (req->bufs != req->bufsml)
      uv__free(req->bufs);
    req->bufs = NULL;

    if (req->send_cb == NULL)
      continue;

    /* req->status >= 0 == bytes written
     * req->status <  0 == errno
     */
    if (req->status >= 0)
      req->send_cb(req, 0);
    else
      req->send_cb(req, req->status);
  }

  if (QUEUE_EMPTY(&handle->write_queue)) {
    /* Pending queue and completion queue empty, stop watcher. */
    uv__io_stop(handle->loop, &handle->io_watcher, POLLOUT);
    if (!uv__io_active(&handle->io_watcher, POLLIN))
      uv__handle_stop(handle);
  }

  handle->flags &= ~UV_HANDLE_UDP_PROCESSING;
}


static void uv__udp_io(uv_loop_t* loop, uv__io_t* w, unsigned int revents) {
  uv_udp_t* handle;

  handle = container_of(w, uv_udp_t, io_watcher);
  assert(handle->type == UV_UDP);

  if (revents & POLLIN)
#if defined(DISCORD_ENABLE_RECVMMSG)
    if (handle->use_recvmmsg) {
      uv__udp_recvmmsg(handle);
    } else {
      uv__udp_recvmsg(handle);
    }
#else
    uv__udp_recvmsg(handle);
#endif

  if (revents & POLLOUT) {
#if defined(DISCORD_ENABLE_SENDMMSG)
    if (handle->use_sendmmsg) {
      uv__udp_sendmmsg(handle);
    } else
#endif
    {
      uv__udp_sendmsg(handle);
    }
    uv__udp_run_completed(handle);
  }
}


static void uv__udp_recvmsg(uv_udp_t* handle) {
  struct sockaddr_storage peer;
  struct msghdr h;
  ssize_t nread;
  uv_buf_t buf;
  int flags;
  int count;

  assert(handle->recv_cb != NULL);
  assert(handle->alloc_cb != NULL);

  /* Prevent loop starvation when the data comes in as fast as (or faster than)
   * we can read it. XXX Need to rearm fd if we switch to edge-triggered I/O.
   */
  count = 32;

  memset(&h, 0, sizeof(h));
  h.msg_name = &peer;

  do {
    buf = uv_buf_init(NULL, 0);
    handle->alloc_cb((uv_handle_t*) handle, 64 * 1024, &buf);
    if (buf.base == NULL || buf.len == 0) {
      handle->recv_cb(handle, UV_ENOBUFS, &buf, NULL, 0);
      return;
    }
    assert(buf.base != NULL);

    h.msg_namelen = sizeof(peer);
    h.msg_iov = (void*) &buf;
    h.msg_iovlen = 1;

    do {
      nread = recvmsg(handle->io_watcher.fd, &h, 0);
    }
    while (nread == -1 && errno == EINTR);

    if (nread == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        handle->recv_cb(handle, 0, &buf, NULL, 0);
      else
        handle->recv_cb(handle, UV__ERR(errno), &buf, NULL, 0);
    }
    else {
      const struct sockaddr *addr;
      if (h.msg_namelen == 0)
        addr = NULL;
      else
        addr = (const struct sockaddr*) &peer;

      flags = 0;
      if (h.msg_flags & MSG_TRUNC)
        flags |= UV_UDP_PARTIAL;

      handle->recv_cb(handle, nread, &buf, addr, flags);
    }
  }
  /* recv_cb callback may decide to pause or close the handle */
  while (nread != -1
      && count-- > 0
      && handle->io_watcher.fd != -1
      && handle->recv_cb != NULL);
}

#if defined(DISCORD_ENABLE_RECVMMSG)
static void uv__udp_recvmmsg(uv_udp_t* handle) {
  struct sockaddr_storage peer;
  struct mmsghdr hdr;
  ssize_t nread;
  uv_buf_t buf;
  int flags;
  int count;

  assert(handle->recv_cb != NULL);
  assert(handle->alloc_cb != NULL);

  memset(&hdr, 0, sizeof(hdr));
  hdr.msg_hdr.msg_name = &peer;

  printf("recvmmsg\n");

  do {
    handle->alloc_cb((uv_handle_t*) handle, 64 * 1024, &buf);
    if (buf.len == 0) {
      handle->recv_cb(handle, UV_ENOBUFS, &buf, NULL, 0);
      return;
    }
    assert(buf.base != NULL);

    hdr.msg_hdr.msg_namelen = sizeof(peer);
    hdr.msg_hdr.msg_iov = (void*) &buf;
    hdr.msg_hdr.msg_iovlen = 1;

    do {
      nread = recvmmsg(handle->io_watcher.fd, &hdr, 1, MSG_DONTWAIT, NULL);
    }
    while (nread == -1 && errno == EINTR);

    if (nread == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        handle->recv_cb(handle, 0, &buf, NULL, 0);
      else
        handle->recv_cb(handle, -errno, &buf, NULL, 0);
    }
    else {
      const struct sockaddr *addr;
      if (hdr.msg_hdr.msg_namelen == 0)
        addr = NULL;
      else
        addr = (const struct sockaddr*) &peer;

      flags = 0;
      if (hdr.msg_hdr.msg_flags & MSG_TRUNC)
        flags |= UV_UDP_PARTIAL;

      handle->recv_cb(handle, hdr.msg_len, &buf, addr, flags);
    }
  }
  /* recv_cb callback may decide to pause or close the handle */
  while (nread != -1
      && count-- > 0
      && handle->io_watcher.fd != -1
      && handle->recv_cb != NULL);
}
#endif

static void uv__udp_sendmsg(uv_udp_t* handle) {
  uv_udp_send_t* req;
  QUEUE* q;
  struct msghdr h;
  ssize_t size;

  while (!QUEUE_EMPTY(&handle->write_queue)) {
    q = QUEUE_HEAD(&handle->write_queue);
    assert(q != NULL);

    req = QUEUE_DATA(q, uv_udp_send_t, queue);
    assert(req != NULL);

    memset(&h, 0, sizeof h);
    if (req->addr.ss_family == AF_UNSPEC) {
      h.msg_name = NULL;
      h.msg_namelen = 0;
    } else {
      h.msg_name = &req->addr;
      if (req->addr.ss_family == AF_INET6)
        h.msg_namelen = sizeof(struct sockaddr_in6);
      else if (req->addr.ss_family == AF_INET)
        h.msg_namelen = sizeof(struct sockaddr_in);
      else if (req->addr.ss_family == AF_UNIX)
        h.msg_namelen = sizeof(struct sockaddr_un);
      else {
        assert(0 && "unsupported address family");
        abort();
      }
    }
    h.msg_iov = (struct iovec*) req->bufs;
    h.msg_iovlen = req->nbufs;

    do {
      size = sendmsg(handle->io_watcher.fd, &h, 0);
    } while (size == -1 && errno == EINTR);

    if (size == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
        break;
    }

    req->status = (size == -1 ? UV__ERR(errno) : size);

    /* Sending a datagram is an atomic operation: either all data
     * is written or nothing is (and EMSGSIZE is raised). That is
     * why we don't handle partial writes. Just pop the request
     * off the write queue and onto the completed queue, done.
     */
    QUEUE_REMOVE(&req->queue);
    QUEUE_INSERT_TAIL(&handle->write_completed_queue, &req->queue);
    uv__io_feed(handle->loop, &handle->io_watcher);
  }
}

#if defined(DISCORD_ENABLE_SENDMMSG)
static void uv__udp_sendmmsg(uv_udp_t* handle) {
  QUEUE* node;
  struct msghdr* hdr;
  struct mmsghdr hdrs[DISCORD_SENDMMSG_BATCHSIZE];
  uv_udp_send_t* reqs[DISCORD_SENDMMSG_BATCHSIZE];
  ssize_t send_cnt;
  ssize_t send_res;
  ssize_t i;

  do {
    send_cnt = 0;

    QUEUE_FOREACH(node, &handle->write_queue) {
      assert(node != NULL);

      reqs[send_cnt] = QUEUE_DATA(node, uv_udp_send_t, queue);
      assert(reqs[send_cnt] != NULL);

      memset(&hdrs[send_cnt], 0, sizeof hdrs[send_cnt]);
      hdr = &hdrs[send_cnt].msg_hdr;
      hdr->msg_name = &reqs[send_cnt]->addr;
      hdr->msg_namelen = (reqs[send_cnt]->addr.ss_family == AF_INET6 ?
        sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
      hdr->msg_iov = (struct iovec*) reqs[send_cnt]->bufs;
      hdr->msg_iovlen = reqs[send_cnt]->nbufs;

      if (++send_cnt == DISCORD_SENDMMSG_BATCHSIZE) {
        break;
      }
    }

    do {
      send_res = sendmmsg(handle->io_watcher.fd, hdrs, send_cnt, 0);
    } while (send_res == -1 && errno == EINTR);

    if (send_res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;

    send_cnt = (send_res == -1) ? send_cnt : send_res;

    for (i = 0; i < send_cnt; ++i) {
      reqs[i]->status = (send_res == -1 ? -errno : (ssize_t)hdrs[i].msg_len);

      QUEUE_REMOVE(&reqs[i]->queue);
      QUEUE_INSERT_TAIL(&handle->write_completed_queue, &reqs[i]->queue);
      uv__io_feed(handle->loop, &handle->io_watcher);
    }
  } while (!QUEUE_EMPTY(&handle->write_queue));
}
#endif // DISCORD_ENABLE_SENDMMSG


/* On the BSDs, SO_REUSEPORT implies SO_REUSEADDR but with some additional
 * refinements for programs that use multicast.
 *
 * Linux as of 3.9 has a SO_REUSEPORT socket option but with semantics that
 * are different from the BSDs: it _shares_ the port rather than steal it
 * from the current listener.  While useful, it's not something we can emulate
 * on other platforms so we don't enable it.
 *
 * zOS does not support getsockname with SO_REUSEPORT option when using
 * AF_UNIX.
 */
static int uv__set_reuse(int fd) {
  int yes;
  yes = 1;

#if defined(SO_REUSEPORT) && defined(__MVS__)
  struct sockaddr_in sockfd;
  unsigned int sockfd_len = sizeof(sockfd);
  if (getsockname(fd, (struct sockaddr*) &sockfd, &sockfd_len) == -1)
      return UV__ERR(errno);
  if (sockfd.sin_family == AF_UNIX) {
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
      return UV__ERR(errno);
  } else {
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)))
       return UV__ERR(errno);
  }
#elif defined(SO_REUSEPORT) && !defined(__linux__)
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)))
    return UV__ERR(errno);
#else
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)))
    return UV__ERR(errno);
#endif

  return 0;
}


int uv__udp_bind(uv_udp_t* handle,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 unsigned int flags) {
  int err;
  int yes;
  int fd;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_bind(handle, addr, addrlen, flags);
  }
#endif

  /* Check for bad flags. */
  if (flags & ~(UV_UDP_IPV6ONLY | UV_UDP_REUSEADDR))
    return UV_EINVAL;

  /* Cannot set IPv6-only mode on non-IPv6 socket. */
  if ((flags & UV_UDP_IPV6ONLY) && addr->sa_family != AF_INET6)
    return UV_EINVAL;

  fd = handle->io_watcher.fd;
  if (fd == -1) {
    err = uv__socket(addr->sa_family, SOCK_DGRAM, 0);
    if (err < 0)
      return err;
    fd = err;
    handle->io_watcher.fd = fd;
  }

  if (flags & UV_UDP_REUSEADDR) {
    err = uv__set_reuse(fd);
    if (err)
      return err;
  }

  if (flags & UV_UDP_IPV6ONLY) {
#ifdef IPV6_V6ONLY
    yes = 1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof yes) == -1) {
      err = UV__ERR(errno);
      return err;
    }
#else
    err = UV_ENOTSUP;
    return err;
#endif
  }

  if (bind(fd, addr, addrlen)) {
    err = UV__ERR(errno);
    if (errno == EAFNOSUPPORT)
      /* OSX, other BSDs and SunoS fail with EAFNOSUPPORT when binding a
       * socket created with AF_INET to an AF_INET6 address or vice versa. */
      err = UV_EINVAL;
    return err;
  }

  if (addr->sa_family == AF_INET6)
    handle->flags |= UV_HANDLE_IPV6;

  handle->flags |= UV_HANDLE_BOUND;
  return 0;
}


static int uv__udp_maybe_deferred_bind(uv_udp_t* handle,
                                       int domain,
                                       unsigned int flags) {
  union {
    struct sockaddr_in6 in6;
    struct sockaddr_in in;
    struct sockaddr addr;
  } taddr;
  socklen_t addrlen;

  if (handle->io_watcher.fd != -1)
    return 0;

  switch (domain) {
  case AF_INET:
  {
    struct sockaddr_in* addr = &taddr.in;
    memset(addr, 0, sizeof *addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = INADDR_ANY;
    addrlen = sizeof *addr;
    break;
  }
  case AF_INET6:
  {
    struct sockaddr_in6* addr = &taddr.in6;
    memset(addr, 0, sizeof *addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_any;
    addrlen = sizeof *addr;
    break;
  }
  default:
    assert(0 && "unsupported address family");
    abort();
  }

  return uv__udp_bind(handle, &taddr.addr, addrlen, flags);
}


int uv__udp_connect(uv_udp_t* handle,
                    const struct sockaddr* addr,
                    unsigned int addrlen) {
  int err;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_connect(handle, addr, addrlen);
  }
#endif

  err = uv__udp_maybe_deferred_bind(handle, addr->sa_family, 0);
  if (err)
    return err;

  do {
    errno = 0;
    err = connect(handle->io_watcher.fd, addr, addrlen);
  } while (err == -1 && errno == EINTR);

  if (err)
    return UV__ERR(errno);

  handle->flags |= UV_HANDLE_UDP_CONNECTED;

  return 0;
}


int uv__udp_disconnect(uv_udp_t* handle) {
    int r;
    struct sockaddr addr;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_disconnect(handle);
  }
#endif

    memset(&addr, 0, sizeof(addr));

    addr.sa_family = AF_UNSPEC;

    do {
      errno = 0;
      r = connect(handle->io_watcher.fd, &addr, sizeof(addr));
    } while (r == -1 && errno == EINTR);

    if (r == -1 && errno != EAFNOSUPPORT)
      return UV__ERR(errno);

    handle->flags &= ~UV_HANDLE_UDP_CONNECTED;
    return 0;
}


int uv__udp_send(uv_udp_send_t* req,
                 uv_udp_t* handle,
                 const uv_buf_t bufs[],
                 unsigned int nbufs,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 uv_udp_send_cb send_cb) {
  int err;
  int empty_queue;
  int immediate;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_send(req, handle, bufs, nbufs, addr, addrlen, send_cb);
  }
#endif

  assert(nbufs > 0);

  if (addr) {
    err = uv__udp_maybe_deferred_bind(handle, addr->sa_family, 0);
    if (err)
      return err;
  }

  /* It's legal for send_queue_count > 0 even when the write_queue is empty;
   * it means there are error-state requests in the write_completed_queue that
   * will touch up send_queue_size/count later.
   */
  empty_queue = (handle->send_queue_count == 0);

  uv__req_init(handle->loop, req, UV_UDP_SEND);
  assert(addrlen <= sizeof(req->addr));
  if (addr == NULL)
    req->addr.ss_family = AF_UNSPEC;
  else
    memcpy(&req->addr, addr, addrlen);
  req->send_cb = send_cb;
  req->handle = handle;
  req->nbufs = nbufs;

  req->bufs = req->bufsml;
  if (nbufs > ARRAY_SIZE(req->bufsml))
    req->bufs = uv__malloc(nbufs * sizeof(bufs[0]));

  if (req->bufs == NULL) {
    uv__req_unregister(handle->loop, req);
    return UV_ENOMEM;
  }

  memcpy(req->bufs, bufs, nbufs * sizeof(bufs[0]));
  handle->send_queue_size += uv__count_bufs(req->bufs, req->nbufs);
  handle->send_queue_count++;
  QUEUE_INSERT_TAIL(&handle->write_queue, &req->queue);
  uv__handle_start(handle);

  immediate = empty_queue && !(handle->flags & UV_HANDLE_UDP_PROCESSING);
#if defined(DISCORD_ENABLE_SENDMMSG)
  immediate = immediate && !handle->use_sendmmsg;
#endif
  
  if (immediate) {
    uv__udp_sendmsg(handle);

    /* `uv__udp_sendmsg` may not be able to do non-blocking write straight
     * away. In such cases the `io_watcher` has to be queued for asynchronous
     * write.
     */
    if (!QUEUE_EMPTY(&handle->write_queue))
      uv__io_start(handle->loop, &handle->io_watcher, POLLOUT);
  } else {
    uv__io_start(handle->loop, &handle->io_watcher, POLLOUT);
  }

  return 0;
}


int uv__udp_try_send(uv_udp_t* handle,
                     const uv_buf_t bufs[],
                     unsigned int nbufs,
                     const struct sockaddr* addr,
                     unsigned int addrlen) {
  int err;
  struct msghdr h;
  ssize_t size;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_try_send(handle, bufs, nbufs, addr, addrlen);
  }
#endif

  assert(nbufs > 0);

  /* already sending a message */
  if (handle->send_queue_count != 0)
    return UV_EAGAIN;

  if (addr) {
    err = uv__udp_maybe_deferred_bind(handle, addr->sa_family, 0);
    if (err)
      return err;
  } else {
    assert(handle->flags & UV_HANDLE_UDP_CONNECTED);
  }

  memset(&h, 0, sizeof h);
  h.msg_name = (struct sockaddr*) addr;
  h.msg_namelen = addrlen;
  h.msg_iov = (struct iovec*) bufs;
  h.msg_iovlen = nbufs;

  do {
    size = sendmsg(handle->io_watcher.fd, &h, 0);
  } while (size == -1 && errno == EINTR);

  if (size == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
      return UV_EAGAIN;
    else
      return UV__ERR(errno);
  }

  return size;
}


static int uv__udp_set_membership4(uv_udp_t* handle,
                                   const struct sockaddr_in* multicast_addr,
                                   const char* interface_addr,
                                   uv_membership membership) {
  struct ip_mreq mreq;
  int optname;
  int err;

  memset(&mreq, 0, sizeof mreq);

  if (interface_addr) {
    err = uv_inet_pton(AF_INET, interface_addr, &mreq.imr_interface.s_addr);
    if (err)
      return err;
  } else {
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  }

  mreq.imr_multiaddr.s_addr = multicast_addr->sin_addr.s_addr;

  switch (membership) {
  case UV_JOIN_GROUP:
    optname = IP_ADD_MEMBERSHIP;
    break;
  case UV_LEAVE_GROUP:
    optname = IP_DROP_MEMBERSHIP;
    break;
  default:
    return UV_EINVAL;
  }

  if (setsockopt(handle->io_watcher.fd,
                 IPPROTO_IP,
                 optname,
                 &mreq,
                 sizeof(mreq))) {
#if defined(__MVS__)
  if (errno == ENXIO)
    return UV_ENODEV;
#endif
    return UV__ERR(errno);
  }

  return 0;
}


static int uv__udp_set_membership6(uv_udp_t* handle,
                                   const struct sockaddr_in6* multicast_addr,
                                   const char* interface_addr,
                                   uv_membership membership) {
  int optname;
  struct ipv6_mreq mreq;
  struct sockaddr_in6 addr6;

  memset(&mreq, 0, sizeof mreq);

  if (interface_addr) {
    if (uv_ip6_addr(interface_addr, 0, &addr6))
      return UV_EINVAL;
    mreq.ipv6mr_interface = addr6.sin6_scope_id;
  } else {
    mreq.ipv6mr_interface = 0;
  }

  mreq.ipv6mr_multiaddr = multicast_addr->sin6_addr;

  switch (membership) {
  case UV_JOIN_GROUP:
    optname = IPV6_ADD_MEMBERSHIP;
    break;
  case UV_LEAVE_GROUP:
    optname = IPV6_DROP_MEMBERSHIP;
    break;
  default:
    return UV_EINVAL;
  }

  if (setsockopt(handle->io_watcher.fd,
                 IPPROTO_IPV6,
                 optname,
                 &mreq,
                 sizeof(mreq))) {
#if defined(__MVS__)
  if (errno == ENXIO)
    return UV_ENODEV;
#endif
    return UV__ERR(errno);
  }

  return 0;
}


int uv_udp_init_ex(uv_loop_t* loop, uv_udp_t* handle, unsigned int flags) {
  int domain;
  int err;
  int fd;

#if defined(DISCORD_ENABLE_NETMAP)
  if (flags & UV_UDP_DISCORD_USE_NETMAP) {
    return uv__udp_netmap_init_handle(loop, handle, flags);
  }
#endif

  /* Use the lower 8 bits for the domain */
  domain = flags & 0xFF;
  if (domain != AF_INET && domain != AF_INET6 && domain != AF_UNSPEC)
    return UV_EINVAL;

  if (domain != AF_UNSPEC) {
    err = uv__socket(domain, SOCK_DGRAM, 0);
    if (err < 0)
      return err;
    fd = err;
  } else {
    fd = -1;
  }

  uv__handle_init(loop, (uv_handle_t*)handle, UV_UDP);
  handle->alloc_cb = NULL;
  handle->recv_cb = NULL;
  handle->send_queue_size = 0;
  handle->send_queue_count = 0;
  handle->use_sendmmsg = flags & UV_UDP_DISCORD_USE_SENDMMSG;
  handle->use_recvmmsg = flags & UV_UDP_DISCORD_USE_RECVMMSG;
  handle->use_netmap = 0;
  uv__io_init(&handle->io_watcher, uv__udp_io, fd);
  QUEUE_INIT(&handle->write_queue);
  QUEUE_INIT(&handle->write_completed_queue);

  return 0;
}


int uv_udp_init(uv_loop_t* loop, uv_udp_t* handle) {
  return uv_udp_init_ex(loop, handle, AF_UNSPEC);
}


int uv_udp_open(uv_udp_t* handle, uv_os_sock_t sock) {
  int err;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_open(handle, sock);
  }
#endif

  /* Check for already active socket. */
  if (handle->io_watcher.fd != -1)
    return UV_EBUSY;

  if (uv__fd_exists(handle->loop, sock))
    return UV_EEXIST;

  err = uv__nonblock(sock, 1);
  if (err)
    return err;

  err = uv__set_reuse(sock);
  if (err)
    return err;

  handle->io_watcher.fd = sock;
  if (uv__udp_is_connected(handle))
    handle->flags |= UV_HANDLE_UDP_CONNECTED;

  return 0;
}


int uv_udp_set_membership(uv_udp_t* handle,
                          const char* multicast_addr,
                          const char* interface_addr,
                          uv_membership membership) {
  int err;
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_set_membership(handle, multicast_addr, interface_addr, membership);
  }
#endif

  if (uv_ip4_addr(multicast_addr, 0, &addr4) == 0) {
    err = uv__udp_maybe_deferred_bind(handle, AF_INET, UV_UDP_REUSEADDR);
    if (err)
      return err;
    return uv__udp_set_membership4(handle, &addr4, interface_addr, membership);
  } else if (uv_ip6_addr(multicast_addr, 0, &addr6) == 0) {
    err = uv__udp_maybe_deferred_bind(handle, AF_INET6, UV_UDP_REUSEADDR);
    if (err)
      return err;
    return uv__udp_set_membership6(handle, &addr6, interface_addr, membership);
  } else {
    return UV_EINVAL;
  }
}

static int uv__setsockopt(uv_udp_t* handle,
                         int option4,
                         int option6,
                         const void* val,
                         size_t size) {
  int r;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_setsockopt(handle, option4, option6, val, size);
  }
#endif

  if (handle->flags & UV_HANDLE_IPV6)
    r = setsockopt(handle->io_watcher.fd,
                   IPPROTO_IPV6,
                   option6,
                   val,
                   size);
  else
    r = setsockopt(handle->io_watcher.fd,
                   IPPROTO_IP,
                   option4,
                   val,
                   size);
  if (r)
    return UV__ERR(errno);

  return 0;
}

static int uv__setsockopt_maybe_char(uv_udp_t* handle,
                                     int option4,
                                     int option6,
                                     int val) {
#if defined(__sun) || defined(_AIX) || defined(__MVS__)
  char arg = val;
#elif defined(__OpenBSD__)
  unsigned char arg = val;
#else
  int arg = val;
#endif

  if (val < 0 || val > 255)
    return UV_EINVAL;

  return uv__setsockopt(handle, option4, option6, &arg, sizeof(arg));
}


int uv_udp_set_broadcast(uv_udp_t* handle, int on) {
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_set_broadcast(handle, on);
  }
#endif

  if (setsockopt(handle->io_watcher.fd,
                 SOL_SOCKET,
                 SO_BROADCAST,
                 &on,
                 sizeof(on))) {
    return UV__ERR(errno);
  }

  return 0;
}


int uv_udp_set_ttl(uv_udp_t* handle, int ttl) {
  if (ttl < 1 || ttl > 255)
    return UV_EINVAL;

#if defined(__MVS__)
  if (!(handle->flags & UV_HANDLE_IPV6))
    return UV_ENOTSUP;  /* zOS does not support setting ttl for IPv4 */
#endif

/*
 * On Solaris and derivatives such as SmartOS, the length of socket options
 * is sizeof(int) for IP_TTL and IPV6_UNICAST_HOPS,
 * so hardcode the size of these options on this platform,
 * and use the general uv__setsockopt_maybe_char call on other platforms.
 */
#if defined(__sun) || defined(_AIX) || defined(__OpenBSD__) || \
    defined(__MVS__)

  return uv__setsockopt(handle,
                        IP_TTL,
                        IPV6_UNICAST_HOPS,
                        &ttl,
                        sizeof(ttl));

#else /* !(defined(__sun) || defined(_AIX) || defined (__OpenBSD__) ||
           defined(__MVS__)) */

  return uv__setsockopt_maybe_char(handle,
                                   IP_TTL,
                                   IPV6_UNICAST_HOPS,
                                   ttl);

#endif /* defined(__sun) || defined(_AIX) || defined (__OpenBSD__) ||
          defined(__MVS__) */
}


int uv_udp_set_multicast_ttl(uv_udp_t* handle, int ttl) {
/*
 * On Solaris and derivatives such as SmartOS, the length of socket options
 * is sizeof(int) for IPV6_MULTICAST_HOPS and sizeof(char) for
 * IP_MULTICAST_TTL, so hardcode the size of the option in the IPv6 case,
 * and use the general uv__setsockopt_maybe_char call otherwise.
 */
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_set_multicast_ttl(handle, ttl);
  }
#endif

#if defined(__sun) || defined(_AIX) || defined(__OpenBSD__) || \
    defined(__MVS__)
  if (handle->flags & UV_HANDLE_IPV6)
    return uv__setsockopt(handle,
                          IP_MULTICAST_TTL,
                          IPV6_MULTICAST_HOPS,
                          &ttl,
                          sizeof(ttl));
#endif /* defined(__sun) || defined(_AIX) || defined(__OpenBSD__) || \
    defined(__MVS__) */

  return uv__setsockopt_maybe_char(handle,
                                   IP_MULTICAST_TTL,
                                   IPV6_MULTICAST_HOPS,
                                   ttl);
}


int uv_udp_set_multicast_loop(uv_udp_t* handle, int on) {
/*
 * On Solaris and derivatives such as SmartOS, the length of socket options
 * is sizeof(int) for IPV6_MULTICAST_LOOP and sizeof(char) for
 * IP_MULTICAST_LOOP, so hardcode the size of the option in the IPv6 case,
 * and use the general uv__setsockopt_maybe_char call otherwise.
 */
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_set_multicast_loop(handle, on);
  }
#endif

#if defined(__sun) || defined(_AIX) || defined(__OpenBSD__) || \
    defined(__MVS__) 
  if (handle->flags & UV_HANDLE_IPV6)
    return uv__setsockopt(handle,
                          IP_MULTICAST_LOOP,
                          IPV6_MULTICAST_LOOP,
                          &on,
                          sizeof(on));
#endif /* defined(__sun) || defined(_AIX) ||defined(__OpenBSD__) ||
    defined(__MVS__) */

  return uv__setsockopt_maybe_char(handle,
                                   IP_MULTICAST_LOOP,
                                   IPV6_MULTICAST_LOOP,
                                   on);
}

int uv_udp_set_multicast_interface(uv_udp_t* handle, const char* interface_addr) {
  struct sockaddr_storage addr_st;
  struct sockaddr_in* addr4;
  struct sockaddr_in6* addr6;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_set_multicast_interface(handle, interface_addr);
  }
#endif

  addr4 = (struct sockaddr_in*) &addr_st;
  addr6 = (struct sockaddr_in6*) &addr_st;

  if (!interface_addr) {
    memset(&addr_st, 0, sizeof addr_st);
    if (handle->flags & UV_HANDLE_IPV6) {
      addr_st.ss_family = AF_INET6;
      addr6->sin6_scope_id = 0;
    } else {
      addr_st.ss_family = AF_INET;
      addr4->sin_addr.s_addr = htonl(INADDR_ANY);
    }
  } else if (uv_ip4_addr(interface_addr, 0, addr4) == 0) {
    /* nothing, address was parsed */
  } else if (uv_ip6_addr(interface_addr, 0, addr6) == 0) {
    /* nothing, address was parsed */
  } else {
    return UV_EINVAL;
  }

  if (addr_st.ss_family == AF_INET) {
    if (setsockopt(handle->io_watcher.fd,
                   IPPROTO_IP,
                   IP_MULTICAST_IF,
                   (void*) &addr4->sin_addr,
                   sizeof(addr4->sin_addr)) == -1) {
      return UV__ERR(errno);
    }
  } else if (addr_st.ss_family == AF_INET6) {
    if (setsockopt(handle->io_watcher.fd,
                   IPPROTO_IPV6,
                   IPV6_MULTICAST_IF,
                   &addr6->sin6_scope_id,
                   sizeof(addr6->sin6_scope_id)) == -1) {
      return UV__ERR(errno);
    }
  } else {
    assert(0 && "unexpected address family");
    abort();
  }

  return 0;
}

int uv_udp_set_tos(uv_udp_t* handle, int tos) {
  return uv__setsockopt(handle, IP_TOS, IPV6_TCLASS, &tos, sizeof(tos));
}

int uv_udp_getpeername(const uv_udp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_getpeername(handle, name, namelen);
  }
#endif

  return uv__getsockpeername((const uv_handle_t*) handle,
                             getpeername,
                             name,
                             namelen);
}

int uv_udp_getsockname(const uv_udp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_getsockname(handle, name, namelen);
  }
#endif
  return uv__getsockpeername((const uv_handle_t*) handle,
                             getsockname,
                             name,
                             namelen);
}


int uv__udp_recv_start(uv_udp_t* handle,
                       uv_alloc_cb alloc_cb,
                       uv_udp_recv_cb recv_cb) {
  int err;

#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_recv_start(handle, alloc_cb, recv_cb);
  }
#endif

  if (alloc_cb == NULL || recv_cb == NULL)
    return UV_EINVAL;

  if (uv__io_active(&handle->io_watcher, POLLIN))
    return UV_EALREADY;  /* FIXME(bnoordhuis) Should be UV_EBUSY. */

  err = uv__udp_maybe_deferred_bind(handle, AF_INET, 0);
  if (err)
    return err;

  handle->alloc_cb = alloc_cb;
  handle->recv_cb = recv_cb;

  uv__io_start(handle->loop, &handle->io_watcher, POLLIN);
  uv__handle_start(handle);

  return 0;
}


int uv__udp_recv_stop(uv_udp_t* handle) {
#if defined(DISCORD_ENABLE_NETMAP)
  if (handle->use_netmap) {
    return uv__udp_netmap_recv_stop(handle);
  }
#endif

  uv__io_stop(handle->loop, &handle->io_watcher, POLLIN);

  if (!uv__io_active(&handle->io_watcher, POLLOUT))
    uv__handle_stop(handle);

  handle->alloc_cb = NULL;
  handle->recv_cb = NULL;

  return 0;
}
