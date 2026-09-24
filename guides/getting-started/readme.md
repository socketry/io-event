# Getting Started

This guide explains how to use `io-event` for non-blocking IO.

## Installation

Add the gem to your project:

~~~ bash
$ bundle add io-event
~~~

## Core Concepts

`io-event` has several core concepts:

- A {ruby IO::Event::Selector} implementation which provides the primitive operations for implementation an event loop.
- A {ruby IO::Event::Debug::Selector} which adds extra validations and checks at the expense of performance. You should generally use this during tests.

## Basic Event Loop

This example shows how to perform a blocking operation 

```ruby
require "fiber"
require "io/event"

# Create an I/O event selector controlled by the main Fiber.
selector = IO::Event::Selector.new(Fiber.current)

# input: read end, output: write end.
input, output = IO.pipe

writer = Fiber.new do
	puts "[writer] Writing data"
	
	output.write("Hello World")
	output.close
end

reader = Fiber.new do
	puts "[reader] No data available, waiting for input"
	
	# Suspend this Fiber until the input becomes readable.
	selector.io_wait(
		Fiber.current,
		input,
		IO::READABLE
	)
	
	# Execution resumes here once the selector detects the event.
	puts "[reader] Received: #{input.read.inspect}"
end

puts "[main] Transferring to reader fiber"
reader.transfer

puts "[main] Reader is waiting, but main can keep running"

puts "[main] Transferring to writer fiber"
writer.transfer

puts "[main] Checking for I/O events"
selector.select(1)

puts "[main] Done"

# Results in:
# [main] Transferring to reader fiber
# [reader] No data available, waiting for input
# [main] Reader is waiting, but main can keep running
# [main] Transferring to writer fiber
# [writer] Writing data
# [main] Checking for I/O events
# [reader] Received: "Hello World"
# [main] Done
```

## Shared-Memory Notifications

On Linux with Ruby 4.1 or later, `IO::Event::Futex` provides atomic operations and notifications on an aligned 32-bit word in an `IO::Buffer`:

```ruby
buffer = IO::Buffer.new(8)
first = IO::Event::Futex.new(buffer)
second = IO::Event::Futex.new(buffer, offset: 4)

first.signal # Increment the word and wake one waiter.
first.close
# The allocation remains locked while second still refers to it:
second.close
buffer.free
```

Each futex owns one counted allocation lock, preventing the backing buffer from being freed, resized, or transferred while its address is retained. Slices lock their root allocation. `close` is idempotent and `closed?` reports whether the futex has been released. Operations on closed or uninitialized futexes raise `IOError`; futexes cannot be copied or reinitialized.

A C-backed finalizer releases the lock if the futex is collected without being closed. Prefer explicit `close` for deterministic release. For externally managed memory, the external owner must also keep the storage alive for the entire binding. A finalizer must not indirectly retain its futex through application references attached to the buffer.

`wait(expected)` waits while the word equals `expected`. `IO::Event::Futex.wait_any([[first, expected], ...])` waits on up to `IO::Event::Futex::WAITV_LIMIT` words. Wake-ups are notifications, not ownership of a permit: always recheck application state in a loop. Changing a word alone does not wake waiters; use `signal` or `wake`.

Without a fiber scheduler, waits release the GVL and block the calling thread. With a scheduler, it must implement the optional `futex_wait` or `futex_waitv` hook. The URing selector exposes these methods only when liburing and the running kernel support the corresponding operations; check `respond_to?` before selecting a notification mechanism. An unsupported scheduler raises `NotImplementedError` rather than blocking its event loop.

Closing a futex with pending waits raises `IOError`. Cancel and finish those waits first. Asynchronous cancellation drains the original operation before releasing its references, and vector waits retain a private snapshot of the supplied futexes.

## Debugging

The {ruby IO::Event::Debug::Selector} class adds extra validations and checks at the expense of performance. It can also log all operations. You can use this by setting the following environment variables:

```shell
$ IO_EVENT_SELECTOR_DEBUG=y IO_EVENT_SELECTOR_DEBUG_LOG=/dev/stderr bundle exec ./my_script.rb
```

The format of the log is subject to change, but it may be useful for debugging.
