# Releases

## v1.16.3

  - Handle `IOError` raised while shutting down the pure Ruby interrupt pipe, so `IO::Event::Interrupt#close` does not leak expected shutdown errors from the interrupt fiber.

## v1.16.2

  - Improve timer heap performance by batching scheduled timer insertion, compacting cancelled timers during flush, and avoiding unnecessary heap rebuilds for small incremental inserts.

## v1.16.1

  - Ensure the pure Ruby `Select` selector returns `false`, not `nil`, when `io_wait` resumes without any ready events.

## v1.16.0

  - Use `eventfd` for `URing` cross-thread wakeup, and enable `IORING_SETUP_SINGLE_ISSUER`, `IORING_SETUP_DEFER_TASKRUN`, and `IORING_SETUP_TASKRUN_FLAG`. The waking thread now signals via `eventfd` rather than submitting a `NOP` SQE, which unlocks the single-issuer optimisation, defers task work to the application thread, and lets `select()` skip the `io_uring_get_events()` syscall when no task work is pending.
  - Add support for the `io_close` fiber-scheduler hook (Ruby 4.0+). The `URing` selector performs the close asynchronously via the ring; the `Debug::Selector` and `TestScheduler` wrappers forward to the underlying selector when supported.
  - Improve `WorkerPool` GC compaction support and add proper write barriers, fixing potential use-after-free under compacting GC.
  - Keep blocked scheduler fibers alive during GC by registering them as roots in `TestScheduler#block`, preventing premature collection and the resulting use-after-free crash on resume.
  - Use Ruby's `xmalloc` / `xcalloc` / `xrealloc2` / `xfree` for all internal selector allocations (the per-fiber ready-queue entries in `IO_Event_Selector_ready_push`, and both the backing array and per-element allocations in `IO_Event_Array`). Previously a raw `malloc` paired with a debug-build-only `assert(...)` would silently dereference `NULL` and crash in release builds under memory pressure; the Ruby allocators trigger a GC sweep on pressure and raise `NoMemoryError` / `RangeError` on real failure, so the `-1` return-code paths through `IO_Event_Array_initialize` / `_resize` / `_lookup` and their callers in `epoll.c` / `kqueue.c` / `uring.c` are removed in favour of straight exception propagation.
  - Correctly handle short `io_uring_submit()` results in the `URing` selector. `io_uring_submit()` returns the number of SQEs actually accepted by the kernel and can be short (SQE prep errors, `ENOMEM`, transient `EAGAIN`); the old accounting reset `pending = 0` on any success and silently lost track of unsubmitted SQEs.
  - Enable `IORING_SETUP_SUBMIT_ALL` (kernel 5.18+) on the `URing` selector so the kernel keeps processing the rest of an SQE batch past individual errors, reducing the frequency of short submits in practice.

## v1.15.1

  - Simplify closed-IO handling in the `Select` selector: rely on Ruby 4's `rb_thread_io_close_interrupt` to wake fibers waiting on a descriptor that's been closed, removing a custom error-recovery path that could mis-attribute `IOError` / `Errno::EBADF` to the wrong waiter.

## v1.15.0

  - Add bounds checks, in the unlikely event of a user providing an invalid offset that exceeds the buffer size. This prevents potential memory corruption and ensures safe operation when using buffered IO methods.

## v1.14.4

  - Allow `epoll_pwait2` to be disabled via `--disable-epoll_pwait2`.

## v1.14.3

  - Fix several implementation bugs that could cause deadlocks on blocking writes.

## v1.14.0

### Enhanced `IO::Event::PriorityHeap` with deletion and bulk insertion methods

The {ruby IO::Event::PriorityHeap} now supports efficient element removal and bulk insertion:

  - **`delete(element)`**: Remove a specific element from the heap in O(n) time
  - **`delete_if(&block)`**: Remove elements matching a condition with O(n) amortized bulk deletion
  - **`concat(elements)`**: Add multiple elements efficiently in O(n) time

<!-- end list -->

``` ruby
heap = IO::Event::PriorityHeap.new

# Efficient bulk insertion - O(n) instead of O(n log n)
heap.concat([5, 2, 8, 1, 9, 3])

# Remove specific element
removed = heap.delete(5)  # Returns 5, heap maintains order

# Bulk removal with condition
count = heap.delete_if{|x| x.even?}  # Removes 2, 8 efficiently
```

The `delete_if` and `concat` methods are particularly efficient for bulk operations, using bottom-up heapification to maintain the heap property in O(n) time. This provides significant performance improvements:

  - **Bulk insertion**: O(n log n) → O(n) for adding multiple elements
  - **Bulk deletion**: O(k×n) → O(n) for removing k elements

Both methods maintain the heap invariant and include comprehensive test coverage with edge case validation.

## v1.11.2

  - Fix Windows build.

## v1.11.1

  - Fix `read_nonblock` when using the `URing` selector, which was not handling zero-length reads correctly. This allows reading available data without blocking.

## v1.11.0

### Introduce `IO::Event::WorkerPool` for off-loading blocking operations.

The {ruby IO::Event::WorkerPool} provides a mechanism for executing blocking operations on separate OS threads while properly integrating with Ruby's fiber scheduler and GVL (Global VM Lock) management. This enables true parallelism for CPU-intensive or blocking operations that would otherwise block the event loop.

``` ruby
# Fiber scheduler integration via blocking_operation_wait hook
class MyScheduler
	def initialize
		@worker_pool = IO::Event::WorkerPool.new
	end
	
	def blocking_operation_wait(operation)
		@worker_pool.call(operation)
	end
end

# Usage with automatic offloading
Fiber.set_scheduler(MyScheduler.new)
# Automatically offload `rb_nogvl(..., RB_NOGVL_OFFLOAD_SAFE)` to a background thread:
result = some_blocking_operation()
```

The implementation uses one or more background threads and a list of pending blocking operations. Those operations either execute through to completion or may be cancelled, which executes the "unblock function" provided to `rb_nogvl`.

## v1.10.2

  - Improved consistency of handling closed IO when invoking `#select`.

## v1.10.0

  - `IO::Event::Profiler` is moved to dedicated gem: [fiber-profiler](https://github.com/socketry/fiber-profiler).
  - Perform runtime checks for native selectors to ensure they are supported in the current environment. While compile-time checks determine availability, restrictions like seccomp and SELinux may still prevent them from working.

## v1.9.0

  - Improved `IO::Event::Profiler` for detecting stalls.

## v1.8.0

  - Detecting fibers that are stalling the event loop.

## v1.7.5

  - Fix `process_wait` race condition on EPoll that could cause a hang.
