# URing `io_wait` Connect Investigation

## Context

Tavian reported production `ENOTCONN` behaviour around `TCPSocket.open` with the `io-event` fiber scheduler using the URing backend. The failing code is in `shopify_security_base`:

```ruby
socket = TCPSocket.open(remote_host, remote_port, *args)
ip = socket.remote_address.ip_address   # ← ENOTCONN raised here
```

`remote_address` calls `getpeername(2)`. On Linux, `inet_getname` returns `ENOTCONN` when the socket is in either `TCP_SYN_SENT` (never connected) **or** `TCP_CLOSE` (was connected, then reset).

## Root Cause — Confirmed

**The bug is a race between RST delivery and `getpeername(2)`.**

1. `TCPSocket.open` connects successfully — `wait_connectable` returns 0, socket is `TCP_ESTABLISHED`.
2. The remote server (LLM API proxy, load-balancer, etc.) immediately sends a RST (or SO_LINGER(0) close) after accepting.
3. The RST transitions the socket to `TCP_CLOSE` on the client side.
4. `socket.remote_address.ip_address` calls `getpeername(2)` on a `TCP_CLOSE` socket — **ENOTCONN**.

This race is not specific to the URing backend. The fiber scheduler makes it more visible because `TCPSocket.open` may complete the connect faster relative to application processing, narrowing the window in which the application needs to act before the server RST lands.

### Reproduction

Reproduced on Linux kernel 6.19 (hana.oriontransfer.co.nz) with the stress test at `test/stress/tcp_connect.rb`:

```bash
bundle exec ruby test/stress/tcp_connect.rb 127.0.0.1 0 200 10000 0
```

Server accepts and immediately RSTs via `SO_LINGER(0)`. With 200 concurrent fibers / 10 000 connections, 4 `ENOTCONN` errors were observed. The `DEBUG_IO_WAIT` log confirmed `io_wait` **never** returned `Integer(0)` — every CQE result was `0x201d` (POLLIN|POLLOUT|POLLERR|POLLHUP|POLLRDHUP) with `returned=5`.

## Initial Hypothesis — Ruled Out

The original suspicion was that `io_wait` could return `Integer(0)` due to the kernel delivering a CQE with flags outside the requested mask, causing `wait_connectable` to read `getsockopt(SO_ERROR)` prematurely while the connect was still in progress.

### Why `Integer(0)` is theoretically possible

`io_wait_transfer` currently does:

```c
return RB_INT2NUM(events_from_poll_flags(result & arguments->flags));
```

If the raw CQE result `> 0` but `result & arguments->flags == 0`, the return value is `Integer(0)` rather than `Qfalse`. Ruby's `wait_connectable` treats any non-`false`, non-negative return as a completed wait, then calls `getsockopt(SO_ERROR)`. If that returns `EINPROGRESS` (socket still connecting), `wait_connectable` treats it as success and the unconnected socket is returned.

### Why this does NOT happen in practice for TCP connects

The Linux kernel's `io_uring/poll.c` unconditionally ORs `IO_POLL_UNMASK` into every poll's event set:

```c
// io_uring/poll.c
#define IO_POLL_UNMASK  (EPOLLERR|EPOLLHUP|EPOLLNVAL|EPOLLRDHUP)
poll->events = events | IO_POLL_UNMASK;
```

This is why `POLLRDHUP` (0x2000) appears in `cqe->res` even when not requested — it passes through the kernel's own filter. However, for TCP sockets, `tcp_poll` guarantees `EPOLLRDHUP` is always delivered alongside `EPOLLIN` (they are set in the same statement). The other `IO_POLL_UNMASK` bits (`EPOLLERR`, `EPOLLHUP`) are already in `arguments->flags`. `EPOLLNVAL` is delivered as `-EBADF` (negative) by io_uring, not as a positive bit. Therefore, a TCP connect CQE will always include at least one bit from `arguments->flags`, keeping `returned > 0`.

The stress test on kernel 6.19 confirmed: across 10 000 RST-close connections, no `ZERO`-tagged log line was ever produced.

## Separate Semantic Issue in `poll_flags_from_events`

Although the `Integer(0)` path is not the root cause of the production ENOTCONN, there is still a mismatch worth fixing:

- The kernel's `poll->events` includes `EPOLLRDHUP` via `IO_POLL_UNMASK`.
- io-event's `arguments->flags` does not include `POLLRDHUP`.

This means `POLLRDHUP` arrives in `cqe->res` as an "unexpected" bit and is silently discarded. The correct fix is to align `arguments->flags` with the kernel's actual filter:

```c
// poll_flags_from_events — include POLLRDHUP alongside POLLHUP/POLLERR
flags |= POLLHUP;
flags |= POLLERR;
flags |= POLLRDHUP;   // kernel always adds this via IO_POLL_UNMASK

// events_from_poll_flags — map POLLRDHUP → READABLE (peer closed = EOF)
if (flags & (POLLIN|POLLHUP|POLLERR|POLLRDHUP)) events |= IO_EVENT_READABLE;
```

And handle `POLLNVAL` as the error it is:

```c
if (result & POLLNVAL) {
    rb_syserr_fail(EBADF, "io_wait_transfer:io_uring_poll_add");
}
```

## Fix for the Root Cause

The ENOTCONN must be handled at the application layer in `shopify_security_base`. The `RestrictedTCPSocket` code should rescue `ENOTCONN` from `remote_address` and either retry or treat the connection as failed:

```ruby
socket = TCPSocket.open(remote_host, remote_port, *args)
begin
    ip = socket.remote_address.ip_address
rescue Errno::ENOTCONN
    # Connection was established but immediately reset by the server.
    # Treat this as a connection failure.
    socket.close rescue nil
    raise Errno::ECONNRESET, "Connection reset before peer address could be read"
end
```

Alternatively, the `SO_ERROR` check could be repeated after `getpeername` fails, or the application could avoid calling `getpeername` on a socket that may have been RST'd.

## Production Environment

- **App**: sidekick-server (Falcon fiber scheduler)
- **Cluster**: `apps-us-ce1-dk0` (GKE, us-central1)
- **Node image**: `COS_CONTAINERD`, GKE `1.34.3-gke.1051003`
- **Kernel**: COS milestone 125, Linux **6.12.46–6.12.55**
- **Ruby**: 3.4.4

## Debug Branch

Branch `repro/tcp-connect-enotconn` adds `DEBUG_IO_WAIT = 1` to `uring.c`, logging every `io_wait` completion that carries unexpected flags or returns `Integer(0)`. If deployed to a staging pod, grep stderr for `UNEXPECTED` or `ZERO` to confirm or rule out the `Integer(0)` path in production conditions.
