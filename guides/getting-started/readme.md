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
	puts "[writer] Started"
	puts "[writer] Writing data..."

	output.write("Hello World")
	output.close

	puts "[writer] Finished"
end

reader = Fiber.new do
	puts "[reader] Started"
	puts "[reader] No data available. Waiting for input..."

	# Suspend this Fiber until the input becomes readable.
	selector.io_wait(
		Fiber.current,
		input,
		IO::READABLE
	)

	# Execution resumes here once the selector detects the event.
	puts "[reader] Received: #{input.read.inspect}"
	puts "[reader] Finished"
end

puts "[main] Starting reader"
reader.transfer

puts "[main] The reader is waiting, but I can keep running"

puts "[main] Starting writer"
writer.transfer

puts "[main] Checking for I/O events"
selector.select(1)

puts "[main] Done"

# Results in:
# [main] Starting reader
# [reader] Started
# [reader] No data available. Waiting for input...
# [main] The reader is waiting, but I can keep running
# [main] Starting writer
# [writer] Started
# [writer] Writing data...
# [writer] Finished
# [main] Checking for I/O events
# [reader] Received: "Hello World"
# [reader] Finished
# [main] Done
```

## Debugging

The {ruby IO::Event::Debug::Selector} class adds extra validations and checks at the expense of performance. It can also log all operations. You can use this by setting the following environment variables:

```shell
$ IO_EVENT_SELECTOR_DEBUG=y IO_EVENT_SELECTOR_DEBUG_LOG=/dev/stderr bundle exec ./my_script.rb
```

The format of the log is subject to change, but it may be useful for debugging.
