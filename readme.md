# ![IO::Event](logo.svg)

Provides low level cross-platform primitives for constructing event loops, with support for `select`, `kqueue`, `epoll` and `io_uring`.

[![Development Status](https://github.com/socketry/io-event/workflows/Test/badge.svg)](https://github.com/socketry/io-event/actions?workflow=Test)

## Motivation

The initial proof-of-concept [Async](https://github.com/socketry/async) was built on [NIO4r](https://github.com/socketry/nio4r). It was perfectly acceptable and well tested in production, however being built on `libev` was a little bit limiting. I wanted to directly build my fiber scheduler into the fabric of the event loop, which is what this gem exposes - it is specifically implemented to support building event loops beneath the fiber scheduler interface, providing an efficient C implementation of all the core operations.

## Usage

Please see the [project documentation](https://socketry.github.io/io-event/) for more details.

  - [Getting Started](https://socketry.github.io/io-event/guides/getting-started/index) - This guide explains how to use `io-event` for non-blocking IO.

  - [Futex Notifications](https://socketry.github.io/io-event/guides/futex/index) - This guide explains how to use `IO::Event::Futex` to notify threads or processes when shared state changes, without continuously polling it.

## Releases

Please see the [project releases](https://socketry.github.io/io-event/releases/index) for all releases.

### Unreleased

  - Sign releases with the shared Socketry certificate.

### v1.22.2

  - Prevent nested worker-pool waits by running `WorkerPool#call` in a `Fiber.blocking` scope. Blocking operations triggered inside scheduler callbacks, including finalizers, execute synchronously. Cancellation still drains submitted work, and spurious wakeups no longer cancel operations.
  - Add `IO::Event::Futex` on Linux with Ruby 4.1+, including atomic value operations and blocking and scheduler-aware single and vector waits over shared memory. Futexes retain a counted allocation lock until explicitly closed or finalized; pending waits prevent closing until completion or cancellation has finished.

### v1.22.1

  - Fix an infinite loop in the ready queue flush when a queued fiber is resumed out of band, e.g. by a stale `unblock` racing a timeout, while another fiber re-queues itself on every iteration.
  - Preserve each ready queue flush's boundary with its own placeholder so newly queued fibers are deferred even when earlier entries are removed. Nested native flushes skip other placeholders, processing work queued before their own boundary. Native selectors ignore placeholders when checking readiness and remove them if a fiber raises.

### v1.22.0

  - Reject non-finite timer times (`NaN` and positive or negative infinity), preventing invalid heap ordering and permanently retained timer handles.

### v1.21.1

  - Fix the `URing` completion free-list empty check so its sole entry can be reused instead of unnecessarily allocating a new completion.
  - Keep cancelled `URing` completion records allocated until both the operation and cancellation completions have been processed.

### v1.20.0

  - Add compatibility with Ruby 4.1's fiber scheduler interface version 4. Buffered IO operations now use `(offset, length)`, perform a single transfer of at most `length` bytes, return short transfers directly, and report `-EAGAIN` without waiting. Earlier Ruby versions retain the existing minimum-progress behavior.

### v1.19.5

  - Preserve the original exception or non-local control flow when `IO::Event::WorkerPool` cancellation interrupts a blocked fiber, while still cancelling and draining the in-flight blocking operation before returning control to Ruby.

### v1.19.4

  - Capture `errno` immediately after `epoll_wait` / `kevent`, preventing stale or subsequently clobbered values from raising a spurious `Errno::*` when a native selector wait is interrupted or skipped.

### v1.19.3

  - Prevent `URing`, `EPoll`, and `KQueue` from entering a native wait when a signal exception becomes pending during the GVL transition. On Ruby versions without `RB_NOGVL_PENDING_INTR_FAIL`, the fallback now refreshes pending-interrupt state immediately before releasing the GVL while preserving the caller's exception-delivery timing.

### v1.19.2

  - Use `rb_process_status_for` when available to construct `URing` `process_wait` results directly from `waitid`, avoiding the extra reap syscall previously needed to build a `Process::Status`.

## Contributing

We welcome contributions to this project.

1.  Fork it.
2.  Create your feature branch (`git checkout -b my-new-feature`).
3.  Commit your changes (`git commit -am 'Add some feature'`).
4.  Push to the branch (`git push origin my-new-feature`).
5.  Create new Pull Request.

### Running Tests

To run the test suite:

``` shell
bundle exec bake build
bundle exec sus
```

### Making Releases

To prepare a release pull request:

``` shell
bundle exec bake gem:github:release:patch # or minor or major
```

After review and merge, GitHub Actions publishes the release. See [bake-gem-github](https://github.com/socketry/bake-gem-github) for setup and release instructions.

### Developer Certificate of Origin

In order to protect users of this project, we require all contributors to comply with the [Developer Certificate of Origin](https://developercertificate.org/). This ensures that all contributions are properly licensed and attributed.

### Community Guidelines

This project is best served by a collaborative and respectful environment. Treat each other professionally, respect differing viewpoints, and engage constructively. Harassment, discrimination, or harmful behavior is not tolerated. Communicate clearly, listen actively, and support one another. If any issues arise, please inform the project maintainers.
