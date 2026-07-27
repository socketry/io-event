# ![IO::Event](logo.svg)

Provides low level cross-platform primitives for constructing event loops, with support for `select`, `kqueue`, `epoll` and `io_uring`.

[![Development Status](https://github.com/socketry/io-event/workflows/Test/badge.svg)](https://github.com/socketry/io-event/actions?workflow=Test)

## Motivation

The initial proof-of-concept [Async](https://github.com/socketry/async) was built on [NIO4r](https://github.com/socketry/nio4r). It was perfectly acceptable and well tested in production, however being built on `libev` was a little bit limiting. I wanted to directly build my fiber scheduler into the fabric of the event loop, which is what this gem exposes - it is specifically implemented to support building event loops beneath the fiber scheduler interface, providing an efficient C implementation of all the core operations.

## Usage

Please see the [project documentation](https://socketry.github.io/io-event/) for more details.

  - [Getting Started](https://socketry.github.io/io-event/guides/getting-started/index) - This guide explains how to use `io-event` for non-blocking IO.

## Releases

Please see the [project releases](https://socketry.github.io/io-event/releases/index) for all releases.

### v1.19.4

  - Capture `errno` immediately after `epoll_wait` / `kevent`, preventing stale or subsequently clobbered values from raising a spurious `Errno::*` when a native selector wait is interrupted or skipped.

### v1.19.3

  - Prevent `URing`, `EPoll`, and `KQueue` from entering a native wait when a signal exception becomes pending during the GVL transition. On Ruby versions without `RB_NOGVL_PENDING_INTR_FAIL`, the fallback now refreshes pending-interrupt state immediately before releasing the GVL while preserving the caller's exception-delivery timing.

### v1.19.2

  - Use `rb_process_status_for` when available to construct `URing` `process_wait` results directly from `waitid`, avoiding the extra reap syscall previously needed to build a `Process::Status`.

### v1.19.1

  - Fix `Process.waitall` / `Process.detach` under the `URing` selector: when `io_uring`'s `waitid` reported an error (e.g. `ECHILD` when there are no more children), the `process_wait` hook raised instead of returning the error as a `Process::Status`, so callers that expect `waitpid` to *report* "no more children" rather than raise would fail.

### v1.19.0

  - Use `io_uring_prep_waitid` for `process_wait` in the `URing` selector (Linux 6.7+), waiting for child exit directly in the ring instead of polling on a `pidfd`. The child is reaped via `rb_process_status_wait` (using `WEXITED | WNOWAIT`) to construct a correct `Process::Status`, and `process_wait(-1, ...)` / `process_wait(0, ...)` are now supported.
  - Support waiting for any child or a process group (`pid <= 0`) on all selectors. The `EPoll` (`pidfd_open`) and `KQueue` (`EVFILT_PROC`) selectors can only watch a specific process, so these cases now fall back to a blocking wait on a dedicated thread; joining it is fiber-scheduler aware, so the reactor keeps running.

### v1.18.0

  - **Fixed**: Avoid entering a blocking native selector wait when an interrupt is already pending for the current thread.

### v1.17.0

  - Report inherited selector objects as closed after fork, and avoid closing descriptors they no longer own.

### v1.16.4

  - Correctly implement `Interrupt#signal` so that it is robust enough to be called by `Scheduler#unblock`.

### v1.16.3

  - Handle `IOError` raised while shutting down the pure Ruby interrupt pipe, so `IO::Event::Interrupt#close` does not leak expected shutdown errors from the interrupt fiber.

### v1.16.2

  - Improve timer heap performance by batching scheduled timer insertion, compacting cancelled timers during flush, and avoiding unnecessary heap rebuilds for small incremental inserts.

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

To make a new release:

``` shell
bundle exec bake gem:release:patch # or minor or major
```

### Developer Certificate of Origin

In order to protect users of this project, we require all contributors to comply with the [Developer Certificate of Origin](https://developercertificate.org/). This ensures that all contributions are properly licensed and attributed.

### Community Guidelines

This project is best served by a collaborative and respectful environment. Treat each other professionally, respect differing viewpoints, and engage constructively. Harassment, discrimination, or harmful behavior is not tolerated. Communicate clearly, listen actively, and support one another. If any issues arise, please inform the project maintainers.
