# Futex Notifications

This guide explains how to use `IO::Event::Futex` to notify threads or processes when shared state changes, without continuously polling it.

Shared state describes what changed; the futex tells you to check it. A wake-up does not transfer a message, reserve a worker, or grant ownership of a permit.

## Availability

`IO::Event::Futex` is available on Linux with Ruby 4.1 or later and the required Linux build headers. It uses Ruby's counted buffer locks to retain an aligned, writable 32-bit word. The class itself does not require `io_uring`.

There are two ways to wait:

- Without an active fiber scheduler for the calling fiber, waits release the GVL and block the calling thread using Linux futex syscalls.
- With a scheduler, waits invoke its optional `futex_wait` or `futex_waitv` hook. A missing hook raises `NotImplementedError`; it does not silently block the event loop.

Only the URing selector provides asynchronous futex waits. EPoll, KQueue, and Select do not. URing exposes each method only when the build's liburing and the running kernel support the corresponding operation. Check the selected backend before choosing an IPC mechanism:

```ruby
require "io/event"

selector = IO::Event::Selector.new(Fiber.current)
begin
	futex_available = IO::Event.const_defined?(:Futex, false)
	puts "Single waits: #{futex_available && selector.respond_to?(:futex_wait)}"
	puts "Vector waits: #{futex_available && selector.respond_to?(:futex_waitv)}"
ensure
	selector.close
end
```

Creating a selector does not install a fiber scheduler. A scheduler integration must forward the following hooks to its own selector, and should only expose hooks that selector supports:

| Scheduler hook | Selector call |
| --- | --- |
| `futex_wait(futex, expected)` | `selector.futex_wait(Fiber.current, futex, expected)` |
| `futex_waitv(entries)` | `selector.futex_waitv(Fiber.current, entries)` |

For blocking vector waits, `IO::Event::Futex.respond_to?(:wait_any)` indicates build support, not running-kernel support; the syscall can still raise `Errno::ENOSYS`. Deployment restrictions can also prevent kernel operations. Negotiate capabilities before peers begin waiting, and choose a socket, pipe, or an IPC protocol such as `async-bus` when asynchronous futex waits are unavailable. There is no automatic IPC fallback in `Futex`.

## Binding Shared Memory

Each futex refers to four bytes at a byte offset within an `IO::Buffer`. The address must be aligned to four bytes, and the buffer must have at least four bytes remaining at that offset. The default offset is zero. Constructing a futex preserves the word's existing value.

```ruby
require "io/event"

buffer = IO::Buffer.new(8)
first = IO::Event::Futex.new(buffer)
second = IO::Event::Futex.new(buffer, offset: 4)

begin
	first.value = 0
	second.value = 0
	first.signal
	puts first.value # => 1
ensure
	first.close
	second.close
	buffer.free
end
```

An ordinary `IO::Buffer.new` allocation is suitable for threads in one process. Forking does not make that allocation shared between processes. For IPC, map shared storage instead: for example, each process can use `IO::Buffer.map(file, size)` on the same pre-sized file opened for reading and writing, without `IO::Buffer::PRIVATE`. Bind futexes at matching offsets in that mapping; virtual addresses need not match between processes.

Initialize the shared words once, before peers attach. Attaching peers must not reset live state. Futexes do not discover peers or transport Ruby objects; the application supplies the shared-state layout and synchronization protocol.

## Atomic Operations and Notifications

| Operation | Effect and return value |
| --- | --- |
| `value` | Atomically reads the word. |
| `value = n` | Atomically stores `n`. |
| `increment(amount = 1)` | Adds `amount` and returns the new value. |
| `decrement(amount = 1)` | Subtracts `amount` and returns the new value. |
| `compare_exchange(expected, desired)` | Stores `desired` only if the word equals `expected`; returns whether it succeeded. |
| `wake(count = 1)` | Wakes at most `count` waiters without changing the word; returns the number woken. |
| `signal(count = 1)` | Increments the word by one, then wakes at most `count` waiters; returns the new word value. |

Loads use acquire ordering, stores use release ordering, and read-modify-write operations use acquire-release ordering. A failed compare-and-exchange uses acquire ordering. Arithmetic wraps modulo `2**32`; a notification counter is not an indefinitely increasing event history.

Only `wake` and `signal` notify sleeping waiters. Assignment, increment, decrement, and compare-and-exchange do not wake them. `signal` performs an atomic increment followed by a wake syscall, not one indivisible increment-and-wake operation. Its argument is the waiter count, not the increment amount. Wake counts must be nonnegative integers that fit in a C `int`.

Publish application state before notifying consumers. A futex does not make other memory accesses atomic or provide a queue's synchronization. Use atomic operations or another suitable synchronization protocol for that state, and do not mix concurrent non-atomic buffer accesses with atomic accesses to the futex word.

## Waiting Without Missing Notifications

`futex.wait(expected)` requires an explicit expected value and asks the kernel to wait only if the word still equals it. Comparing the word and beginning the wait is atomic with respect to futex operations. If the value has already changed, the call returns `false` without sleeping; a successful wake returns `true`. Omitting `expected` raises `ArgumentError`.

Always recheck application state after a wait. Wake-ups can be spurious, another consumer may have taken the available work, and waking a waiter does not guarantee FIFO ordering or ownership. System errors raise exceptions. Neither wait API currently accepts a timeout argument.

When the word is a notification counter for separate application state, the consumer must:

1. Read the notification counter.
2. Check or attempt to consume the synchronized application state.
3. If no work is available, wait using the saved counter value.
4. Repeat after the wait returns.

The producer publishes work before incrementing the counter and waking consumers. If a producer publishes between steps 2 and 3, the changed counter prevents the consumer from sleeping.

### Why the Expected Value Is Required

Reading the counter *after* checking for work can miss a notification:

1. The consumer finds the queue empty. The notification counter is `0`.
2. The producer adds work and signals, changing the counter to `1`. Nobody is waiting yet.
3. The consumer reads the counter and gets `1`.
4. The consumer calls `wait(1)`. Since the counter is still `1`, it sleeps despite available work. Without another notification, it can remain asleep indefinitely.

An argument-free `wait` would hide step 3 inside the method. Requiring `expected` makes the snapshot explicit: the consumer should capture `0` before checking the queue, then call `wait(0)`. In the same sequence, the kernel sees that the counter is now `1` and returns immediately. If the consumer starts waiting before the producer signals, the signal wakes it instead.

The required argument does not enforce the ordering by itself. Calling `wait(futex.value)` after checking for work has the same race. The intended sequence is **snapshot, check state, then wait using that snapshot**, followed by another state check after the wait returns.

### Producer and Consumer

This example uses a thread-safe Ruby `Queue` for application state and a futex for notifications. `Queue` already supports blocking `pop`; the explicit wait here demonstrates the protocol you would use with your own shared state. This particular queue is local to one process, not an IPC queue.

```ruby
require "io/event"

buffer = IO::Buffer.new(4)
notification = IO::Event::Futex.new(buffer)
notification.value = 0
queue = Thread::Queue.new

producer = Thread.new do
	["first", "second", "third", nil].each do |message|
		queue << message
		notification.signal
	end
end

begin
	loop do
		# Snapshot before checking application state:
		expected = notification.value
		
		begin
			message = queue.pop(true)
		rescue ThreadError
			notification.wait(expected)
			next
		end
		
		# A nil message marks the end of this example:
		break if message.nil?
		puts message
	end
ensure
	producer.join
	notification.close
	buffer.free
end
```

The queue retains work even when a notification wakes nobody. Notifications may be coalesced; consumers must inspect the state rather than count wake-ups. A 32-bit counter can wrap back to a saved value, so protocols must account for wraparound if a consumer could miss an entire counter cycle between its snapshot and wait.

## Waiting on Several Words

When supported, `IO::Event::Futex.wait_any(entries)` waits on a vector of `[futex, expected]` pairs. Supply between one and `IO::Event::Futex::WAITV_LIMIT` entries. It returns a zero-based index when woken, or `nil` if any word does not match its expected value. The index is a notification hint, not an exhaustive list of changes or a claim on the associated resource.

For example, wait until either of two initially-zero words is nonzero:

```ruby
require "io/event"

buffer = IO::Buffer.new(8)
first = IO::Event::Futex.new(buffer)
second = IO::Event::Futex.new(buffer, offset: 4)
first.value = second.value = 0

producer = Thread.new{second.signal}

begin
	while first.value == 0 && second.value == 0
		IO::Event::Futex.wait_any([[first, 0], [second, 0]])
	end
	puts "At least one word changed."
ensure
	producer.join
	first.close
	second.close
	buffer.free
end
```

For notification counters alongside separate state, snapshot all counters before checking that state, just as with a single wait. Vector waits retain a private snapshot of the supplied futex references; mutating the original entries does not change an already-pending wait.

## Lifetime and Cancellation

Each futex owns one counted allocation lock from construction until `close` or finalization. Slices lock their root allocation, and multiple words can independently retain the same allocation. The lock prevents freeing, resizing, or transferring that allocation; it does not prevent writing shared state.

`close` is idempotent, and `closed?` reports whether the futex has been released. Word operations and waits on closed or uninitialized instances raise `IOError`. Futexes cannot be copied or reinitialized. A finalizer releases the lock if a futex is collected, but explicit `close` makes release deterministic. Avoid application references from the retained buffer back to its futex, which would keep it reachable through the finalizer.

A futex with pending waits cannot be closed: `close` raises `IOError` rather than cancelling or waking those waits. For orderly shutdown:

1. Publish shutdown state and notify the necessary waiters, or cancel them through the scheduler.
2. Let the waits finish, including cancellation cleanup.
3. Close all futexes, then free or unmap the buffer.

URing cancellation drains the original kernel operation before releasing its retained references or wait vector. Keep the event loop running until that cleanup completes. Exceptions still propagate; if a wait is resumed out of band without an exception, it can instead return `false` for a single wait or `nil` for a vector wait. These returns still require rechecking application state.

Allocation locks cannot protect against an external owner releasing memory or another process truncating a mapped file. The application must preserve the underlying storage for every process that can still access it.

## Running the Tests

From a project checkout, run the same task used by the dedicated Linux CI job:

```shell
bundle exec bake test_futex
```

This builds the extension, requires native vector waits and both URing futex-wait methods, and runs the Futex tests. Missing support is an error rather than a skipped test suite. The ordinary test suite remains usable on platforms without Futex support.

## Further Reading

The Linux [futex overview](https://man7.org/linux/man-pages/man2/futex.2.html), [FUTEX_WAIT](https://man7.org/linux/man-pages/man2/FUTEX_WAIT.2const.html), and [FUTEX_WAKE](https://man7.org/linux/man-pages/man2/FUTEX_WAKE.2const.html) documentation describe the underlying shared-memory and notification semantics.
