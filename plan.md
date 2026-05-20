# Performance Plan: Lessons from `carbon_fiber`

Working notes after reviewing [`yaroslav/carbon_fiber`](https://github.com/yaroslav/carbon_fiber) (Zig + libxev fiber scheduler) and reproducing its benchmarks on `hana` (Intel Xeon E3-1240v6, 4c/8t, kernel 6.19, Ruby 4.0.2 +YJIT, io_uring, 3 measured runs).

## Reproduced benchmarks (hana)

`carbon_fiber-0.1.3` (prebuilt gem) vs `async-2.38.1` + `io-event-1.16.0`, run from carbon_fiber's own `benchmarks/` harness:

| Workload | Carbon Fiber | Async/io-event | Δ (hana) | Δ (their README) |
|---|---|---|---|---|
| `http_server` | 33.43k req/s | 29.61k req/s | **+12.9%** | +60% |
| `tcp_echo` | 35.14k ops/s | 30.16k ops/s | **+16.5%** | +64% |
| `http_client_api` | 10.43k req/s | 8.39k req/s | **+24.3%** | +17% |
| `http_client_download` | 4.47k dl/s | 4.22k dl/s | +5.9% | +19% |
| `connection_pool` | 4.97k co/s | 4.58k co/s | +8.5% | +8% |
| `fan_out_gather` | 2.06k cyc/s | 1.93k cyc/s | +6.7% | +5% |
| `db_query_mix` | 1.66k qry/s | 1.60k qry/s | +4.1% | +2% |
| `cascading_timeout` | 4.64k ops/s | 4.35k ops/s | +6.7% | +6% |

Carbon Fiber wins every workload, by +4% to +24%. Their headline +60/+64% numbers don't reproduce on this hardware (probably because Zen 4 on c7a.2xlarge is more scheduler-bound at higher absolute throughput); smaller +5–8% claims reproduce precisely.

## Optimisation catalogue (ranked by expected impact)

### Tier 1 — biggest wins, but need Ruby-side cooperation

#### 1.1 New scheduler hooks: `socket_recvmsg` / `socket_sendmsg`

**The plan**: add explicit socket-only hooks to the Ruby fiber scheduler protocol. With a guaranteed-socket contract on the C side we can implement the userspace fast path cleanly:

```c
// In io-event, when the new hooks land:
ssize_t IO_Event_Selector_URing_socket_recvmsg(...) {
    ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (n >= 0) return n;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
    // Slow path: submit io_uring SQE for recvmsg and yield.
}
```

No `ENOTSOCK` worry, no `fcntl` dance, no per-fd type-caching. The contract is "this fd is a socket" by construction.

This is the cleanest path to the carbon_fiber-style `recvOnce`/`sendOnce` fast path. Carbon Fiber's biggest single win (~half of their `tcp_echo` / `http_server` lead) comes from skipping the io_uring round-trip when data is already buffered, and that's exactly what this enables.

**Effort**: depends on the Ruby side. Once the hooks exist, io-event change is ~200 lines for `recvmsg` + `sendmsg`.

**Expected gain**: 5–10% on `tcp_echo`, `http_server` once Ruby's socket library routes through the new hooks.

---

### Tier 2 — io-event-only changes, no Ruby-side dependency

#### 2.1 Fiber chaining in `IO_Event_Selector_loop_yield`

**Current behaviour**: every user fiber yield goes `user → loop_fiber`; then `select` calls `IO_Event_Selector_ready_flush` which does `loop_fiber → next_user_fiber`. Two `rb_fiber_transfer` calls per scheduling decision.

**Carbon Fiber's trick**: when a user fiber yields, peek the ready queue first. If non-empty, `rb_fiber_transfer` directly to the head of the queue, skipping the loop-fiber round-trip. One transfer per scheduling decision.

```
// Pseudocode for IO_Event_Selector_loop_yield:
if (backend->ready) {
    pop head;
    return rb_fiber_transfer(head->fiber, 0, NULL);
}
return rb_fiber_transfer(backend->loop, 0, NULL);
```

**Risk**: correctness around `rb_fiber_raise` — carbon_fiber needs a `blocked_fibers` tracking set to avoid stranding fibers when a raise bypasses the normal park/resume cycle. We already have `IO_Event_Selector_resume`/`_raise` plumbing; need to audit whether chaining interacts safely with our existing in-flight io_uring completions.

**Effort**: ~50 lines + careful tests around interrupt-during-yield.

**Expected gain**: 5–10% on hot scheduling workloads (`tcp_echo`, `db_query_mix`).

#### 2.2 Opportunistic `io_uring_get_events` CQ peek before yielding to loop_fiber

**Carbon Fiber** (`doTransferToLoop` lines 1122–1135): on io_uring only, when ready queue is empty but `active_waiters > 0`, peek the completion queue once before transferring to loop_fiber. On loopback workloads (tcp_echo, http) the peer's response often lands by the time we reach the yield point — chaining saves a full loop_fiber round-trip.

**Compose with our recent work**: this pairs naturally with the `IORING_SETUP_TASKRUN_FLAG` work we just landed. We already gate `io_uring_get_events()` on `IORING_SQ_TASKRUN`; this is the same syscall used to absorb a free completion.

```
// In loop_yield, before transferring to loop:
if (selector->pending_io > 0) {
    io_uring_get_events(&selector->ring);  // cheap when no work pending
    drain_cqes_into_ready_queue();
    if (backend->ready) goto chain_to_ready;
}
```

**Effort**: ~30 lines. No Ruby-side change.

**Expected gain**: 3–5% on workloads with localhost/short-RTT I/O.

#### 2.3 Push `fileno` extraction into C in `Async::Scheduler::Selector`

Carbon Fiber's `io_wait_object` / `io_read_object` / `io_write_object` accept the `IO` object directly and call `IO_Event_Selector_io_descriptor` (or equivalent) in C, skipping a `respond_to?(:fileno)` + `fileno` method-send pair per call.

io-event's native methods already take `VALUE io`. The waste is on the Async side:

```ruby
# async/lib/async/scheduler/selector/uring.rb (or similar)
def io_read(fiber, io, buffer, length, offset = 0)
  @selector.io_read(fiber, io.fileno, buffer, length, offset)  # <- fileno in Ruby
end
```

**Effort**: trivial — drop the `.fileno` and let the C side extract it.

**Expected gain**: 1–3% on read/write-heavy workloads, mostly from skipping the Ruby method call frame.

#### 2.4 Cache "is_socket" bit in the `Descriptor` struct

If we go ahead with a `recv(MSG_DONTWAIT)` fast path inside `io_read` (i.e. without waiting for the new `socket_recvmsg` hook — see 3.1), the cleanest way to handle ENOTSOCK is to cache the verdict per fd. First call to a new fd costs one wasted recv on a non-socket; every subsequent call goes straight to the right syscall.

The URing selector already maintains a per-fd `Descriptor` struct (for close-watch state); add a `bool known_socket; bool known_not_socket;` pair (3 states: unknown, socket, not-socket).

**Effort**: ~30 lines.

**Expected gain**: makes 3.1 net-positive on mixed socket/pipe workloads. Standalone gain is zero.

---

### Tier 3 — fast path inside existing `io_read` / `io_write` hooks

These are the changes we'd make *without* waiting for new Ruby hooks. They overlap with Tier 1, so revisit after `socket_recvmsg`/`socket_sendmsg` land.

#### 3.1 Userspace `recv(MSG_DONTWAIT)` probe before io_uring submission

**Carbon Fiber's `ioRead`**: try `recv(MSG_DONTWAIT)` first; if `> 0` return immediately, if `ENOTSOCK` fall back to `read(2)`, if `EAGAIN` go to io_uring slow path.

For io-event, the safe order is:
1. If descriptor known-not-socket: skip to `read(2)` probe.
2. Else `recv(MSG_DONTWAIT)`:
   - `> 0`: return (optionally `drainRecv` more — see 3.3).
   - `0`: EOF, return 0.
   - `ENOTSOCK`: mark known-not-socket, fall through to `read(2)`.
   - `EAGAIN`/`EWOULDBLOCK`: fall through to io_uring slow path.
   - Other errno: return as error.

**Risk**: changes observable behaviour (different syscalls show up in strace/audit/seccomp). For mainstream callers it's invisible.

**Effort**: ~80 lines in `uring.c`'s `IO_Event_Selector_URing_io_read` (and symmetric for `io_write`).

**Expected gain**: 8–15% on `tcp_echo`, `http_server`. This is the single biggest win identified.

**Supersedes**: the existing `length == 0` (readpartial) `fcntl` dance — that path becomes one wasted recv instead of two fcntl calls on a non-socket, and zero overhead on a socket.

#### 3.2 Adaptive per-fd probe-skip

After 3.1 is in: track consecutive `EAGAIN` returns per-fd (indexed by `fd & 0xFF` in carbon_fiber, or stored in our `Descriptor` struct). After N (carbon_fiber uses 3) consecutive misses, skip the userspace probe and go straight to io_uring for that slot. Reset on a successful hit.

Without this, long-polling workloads (websocket idle, long-lived keep-alive) pay one wasted `recv` per `io_read`. With it, the probe self-tunes.

**Effort**: ~20 lines on top of 3.1.

**Expected gain**: prevents 3.1 from regressing on long-poll workloads. Standalone: zero.

#### 3.3 `drainRecv` after a successful first recv

After `recvOnce` returns N bytes, opportunistically try one more `recv(MSG_DONTWAIT)` to drain any remaining kernel buffer in the same `io_read` call. Skipped for `length == 0` (readpartial) since the caller only wants what's immediately available.

**Effort**: ~10 lines.

**Expected gain**: 1–2% on streaming workloads where messages span multiple TCP segments.

---

### Tier 4 — smaller, more situational wins

#### 4.1 Combined R|W `io_wait` → poll-writable-only

Carbon Fiber: when `io_wait(events = READABLE | WRITABLE)` is called (common for `connect()` completion), only register a writable poll — writability implies the connect finished, return both flags. Saves one SQE.

**Effort**: trivial. **Expected gain**: marginal except for connect-heavy workloads.

#### 4.2 Skip `MSG_PEEK` readiness probe in `io_wait`

When Ruby calls `io_wait`, the calling code has already seen EAGAIN, so a `MSG_PEEK` "is it ready?" probe is guaranteed to fail. io-event doesn't do this peek today, but worth noting we shouldn't add it.

#### 4.3 Near-deadline busy-spin for sub-3ms timers

Carbon Fiber's `SPIN_THRESHOLD = 3 ms`: for timers that close to firing, busy-spin instead of releasing GVL + re-arming a kernel timer. Specific to GVL release cost.

**Effort**: ~30 lines. **Expected gain**: ~2% on `cascading_timeout`. Probably not worth the complexity for that workload alone.

---

### Tier 5 — DO NOT COPY

#### 5.1 Net::HTTP keep-alive `wait_readable(0)` short-circuit

Carbon Fiber's `Scheduler#poll_io_now` returns `false` unconditionally for `BasicSocket.wait_readable(0)`. This is a deliberate Net::HTTP keep-alive optimisation — skips the MSG_PEEK staleness probe Net::HTTP performs before every request. Comment in the source acknowledges it costs one extra request's latency on a genuinely stale connection.

This accounts for some chunk of their `http_client_api` win. It's *functionally* correct on the happy path but it's a benchmark-tuned shortcut that would be wrong for io-event to copy at the framework level — too aggressive for a general-purpose library.

---

## Suggested order

1. **Land [`socket_recvmsg`/`socket_sendmsg`](https://bugs.ruby-lang.org/) hooks in Ruby + io-event** (1.1). Cleanest foundation, no compromises.
2. **Fiber chaining** (2.1) — biggest scheduler-side win that's independent of the hook work. Needs careful tests.
3. **CQ peek before loop yield** (2.2) — composes with (1.1) and (2.1), small lines-of-code, fits naturally into the post-PR-#166 code path.
4. **`fileno` extraction in C** in Async's adapter (2.3) — trivial change, may as well land alongside.
5. **Userspace fast path inside `io_read`/`io_write`** (3.1 + 3.2 + 2.4) — only if (1.1) is delayed. Otherwise prefer the new hook path.
6. Re-benchmark on hana + c7a.2xlarge after each step.

## Open questions

- Status of `socket_recvmsg`/`socket_sendmsg` hooks upstream — do they need to be designed, or are they already proposed?
- Does Ruby's `BasicSocket` need to opt in, or do we get coverage automatically through `Socket#recvmsg` / `#sendmsg`?
- For (2.1), is there an existing test that exercises `Fiber#raise` against a fiber suspended via `io_wait`? If not, we should write one before changing `loop_yield`.
