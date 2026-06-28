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

### v1.17.0

  - Report inherited selector objects as closed after fork, and avoid closing descriptors they no longer own.

### v1.16.4

  - Correctly implement `Interrupt#signal` so that it is robust enough to be called by `Scheduler#unblock`.

### v1.16.3

  - Handle `IOError` raised while shutting down the pure Ruby interrupt pipe, so `IO::Event::Interrupt#close` does not leak expected shutdown errors from the interrupt fiber.

### v1.16.2

  - Improve timer heap performance by batching scheduled timer insertion, compacting cancelled timers during flush, and avoiding unnecessary heap rebuilds for small incremental inserts.

### v1.16.1

  - Ensure the pure Ruby `Select` selector returns `false`, not `nil`, when `io_wait` resumes without any ready events.

### v1.16.0

  - Use `eventfd` for `URing` cross-thread wakeup, and enable `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_DEFER_TASKRUN`, and `IORING_SETUP_TASKRUN_FLAG`. The waking thread now signals via `eventfd` rather than submitting a `NOP` SQE, which unlocks the single-issuer optimisation, defers task work to the application thread, and lets `select()` skip the `io_uring_get_events()` syscall when no task work is pending.
  - Add support for the `io_close` fiber-scheduler hook (Ruby 4.0+). The `URing` selector performs the close asynchronously via the ring; the `Debug::Selector` and `TestScheduler` wrappers forward to the underlying selector when supported.
  - Improve `WorkerPool` GC compaction support and add proper write barriers, fixing potential use-after-free under compacting GC.
  - Keep blocked scheduler fibers alive during GC by registering them as roots in `TestScheduler#block`, preventing premature collection and the resulting use-after-free crash on resume.
  - Use Ruby's `xmalloc` / `xcalloc` / `xrealloc2` / `xfree` for all internal selector allocations (the per-fiber ready-queue entries in `IO_Event_Selector_ready_push`, and both the backing array and per-element allocations in `IO_Event_Array`). Previously a raw `malloc` paired with a debug-build-only `assert(...)` would silently dereference `NULL` and crash in release builds under memory pressure; the Ruby allocators trigger a GC sweep on pressure and raise `NoMemoryError` / `RangeError` on real failure, so the `-1` return-code paths through `IO_Event_Array_initialize` / `_resize` / `_lookup` and their callers in `epoll.c` / `kqueue.c` / `uring.c` are removed in favour of straight exception propagation.
  - Correctly handle short `io_uring_submit()` results in the `URing` selector. `io_uring_submit()` returns the number of SQEs actually accepted by the kernel and can be short (SQE prep errors, `ENOMEM`, transient `EAGAIN`); the old accounting reset `pending = 0` on any success and silently lost track of unsubmitted SQEs.
  - Enable `IORING_SETUP_SUBMIT_ALL` (kernel 5.18+) on the `URing` selector so the kernel keeps processing the rest of an SQE batch past individual errors, reducing the frequency of short submits in practice.

### v1.15.1

  - Simplify closed-IO handling in the `Select` selector: rely on Ruby 4's `rb_thread_io_close_interrupt` to wake fibers waiting on a descriptor that's been closed, removing a custom error-recovery path that could mis-attribute `IOError` / `Errno::EBADF` to the wrong waiter.

### v1.15.0

  - Add bounds checks, in the unlikely event of a user providing an invalid offset that exceeds the buffer size. This prevents potential memory corruption and ensures safe operation when using buffered IO methods.

### v1.14.4

  - Allow `epoll_pwait2` to be disabled via `--disable-epoll_pwait2`.

### v1.14.3

  - Fix several implementation bugs that could cause deadlocks on blocking writes.

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
