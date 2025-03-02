# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event/selector"

module IO::Event
	# Handles scheduling of fibers. Implements the fiber scheduler interface.
	class Scheduler
		# Raised when an operation is attempted on a closed scheduler.
		class ClosedError < RuntimeError
			# Create a new error.
			#
			# @parameter message [String] The error message.
			def initialize(message = "Scheduler is closed!")
				super
			end
		end
		
		# Create a new scheduler.
		#
		# @parameter parent [Node | Nil] The parent node to use for task hierarchy.
		# @parameter selector [IO::Event::Selector] The selector to use for event handling.
		def initialize(selector = nil)
			@selector = selector || ::IO::Event::Selector.new(Fiber.current)
			
			@interrupted = false
			
			@blocked = 0
			
			@timers = ::IO::Event::Timers.new
		end
		
		# Whether there are any tasks that are currently executing using this scheduler:
		def ready?
			@blocked > 0 or @selector.ready?
		end
		
		# Invoked when the fiber scheduler is being closed due to going out of scope.
		def scheduler_close(error = $!)
			# If the execution context (thread) was handling an exception, we want to exit as quickly as possible:
			unless error
				self.run
			end
		ensure
			self.close
		end
		
		# Terminate all child tasks and close the scheduler.
		def close
			Kernel.raise "Closing scheduler with blocked operations!" if @blocked > 0
			
			# We want `@selector = nil` to be a visible side effect from this point forward, specifically in `#interrupt` and `#unblock`. If the selector is closed, then we don't want to push any fibers to it.
			selector = @selector
			@selector = nil
			
			selector&.close
		end
		
		# @returns [Boolean] Whether the scheduler has been closed.
		# @public Since *Async v1*.
		def closed?
			@selector.nil?
		end
		
		# Schedule a fiber to be resumed on the next iteration of the event loop.
		def fiber(&block)
			fiber = Fiber.new(&block)
			
			self.push(fiber)
			
			return fiber
		end
		
		# Interrupt the event loop and cause it to exit.
		# @asynchronous May be called from any thread.
		def interrupt
			@interrupted = true
			@selector&.wakeup
		end
		
		# Transfer from the calling fiber to the event loop.
		def transfer
			@selector.transfer
		end
		
		# Yield the current fiber and resume it on the next iteration of the event loop.
		def yield
			@selector.yield
		end
		
		# Schedule a fiber (or equivalent object) to be resumed on the next loop through the reactor.
		# @parameter fiber [Fiber | Object] The object to be resumed on the next iteration of the run-loop.
		def push(fiber)
			@selector.push(fiber)
		end
		
		# Raise an exception on a specified fiber with the given arguments.
		#
		# This internally schedules the current fiber to be ready, before raising the exception, so that it will later resume execution.
		#
		# @parameter fiber [Fiber] The fiber to raise the exception on.
		# @parameter *arguments [Array] The arguments to pass to the fiber.
		def raise(...)
			@selector.raise(...)
		end
		
		# Resume execution of the specified fiber.
		#
		# @parameter fiber [Fiber] The fiber to resume.
		# @parameter arguments [Array] The arguments to pass to the fiber.
		def resume(fiber, *arguments)
			@selector.resume(fiber, *arguments)
		end
		
		# Invoked when a fiber tries to perform a blocking operation which cannot continue. A corresponding call {unblock} must be performed to allow this fiber to continue.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May only be called on same thread as fiber scheduler.
		#
		# @parameter blocker [Object] The object that is blocking the fiber.
		# @parameter timeout [Float | Nil] The maximum time to block, or if nil, indefinitely.
		def block(blocker, timeout)
			# $stderr.puts "block(#{blocker}, #{Fiber.current}, #{timeout})"
			fiber = Fiber.current
			
			if timeout
				timer = @timers.after(timeout) do
					if fiber.alive?
						fiber.transfer(false)
					end
				end
			end
			
			begin
				@blocked += 1
				@selector.transfer
			ensure
				@blocked -= 1
			end
		ensure
			timer&.cancel!
		end
		
		# Unblock a fiber that was previously blocked.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May be called from any thread.
		#
		# @parameter blocker [Object] The object that was blocking the fiber.
		# @parameter fiber [Fiber] The fiber to unblock.
		def unblock(blocker, fiber)
			# $stderr.puts "unblock(#{blocker}, #{fiber})"
			
			# This operation is protected by the GVL:
			if selector = @selector
				selector.push(fiber)
				selector.wakeup
			end
		end
		
		# Sleep for the specified duration.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May be non-blocking.
		#
		# @parameter duration [Numeric | Nil] The time in seconds to sleep, or if nil, indefinitely.
		def kernel_sleep(duration = nil)
			if duration
				self.block(nil, duration)
			else
				self.transfer
			end
		end
		
		# Resolve the address of the given hostname.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May be non-blocking.
		#
		# @parameter hostname [String] The hostname to resolve.
		def address_resolve(hostname)
			# On some platforms, hostnames may contain a device-specific suffix (e.g. %en0). We need to strip this before resolving.
			# See <https://github.com/socketry/async/issues/180> for more details.
			hostname = hostname.split("%", 2).first
			::Resolv.getaddresses(hostname)
		end
		
		if IO.method_defined?(:timeout)
			private def get_timeout(io)
				io.timeout
			end
		else
			private def get_timeout(io)
				nil
			end
		end
		
		# Wait for the specified IO to become ready for the specified events.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May be non-blocking.
		#
		# @parameter io [IO] The IO object to wait on.
		# @parameter events [Integer] The events to wait for, e.g. `IO::READABLE`, `IO::WRITABLE`, etc.
		# @parameter timeout [Float | Nil] The maximum time to wait, or if nil, indefinitely.
		def io_wait(io, events, timeout = nil)
			fiber = Fiber.current
			
			if timeout
				# If an explicit timeout is specified, we expect that the user will handle it themselves:
				timer = @timers.after(timeout) do
					fiber.transfer
				end
			elsif timeout = get_timeout(io)
				# Otherwise, if we default to the io's timeout, we raise an exception:
				timer = @timers.after(timeout) do
					fiber.raise(::IO::TimeoutError, "Timeout (#{timeout}s) while waiting for IO to become ready!")
				end
			end
			
			return @selector.io_wait(fiber, io, events)
		ensure
			timer&.cancel!
		end
		
		if ::IO::Event::Support.buffer?
			# Read from the specified IO into the buffer.
			#
			# @public Since *Ruby v3.1* and Ruby with `IO::Buffer` support.
			# @asynchronous May be non-blocking.
			#
			# @parameter io [IO] The IO object to read from.
			# @parameter buffer [IO::Buffer] The buffer to read into.
			# @parameter length [Integer] The minimum number of bytes to read.
			# @parameter offset [Integer] The offset within the buffer to read into.
			def io_read(io, buffer, length, offset = 0)
				fiber = Fiber.current
				
				if timeout = get_timeout(io)
					timer = @timers.after(timeout) do
						fiber.raise(::IO::TimeoutError, "Timeout (#{timeout}s) while waiting for IO to become readable!")
					end
				end
				
				@selector.io_read(fiber, io, buffer, length, offset)
			ensure
				timer&.cancel!
			end
			
			if RUBY_ENGINE != "ruby" || RUBY_VERSION >= "3.3.1"
				# Write the specified buffer to the IO.
				#
				# @public Since *Ruby v3.3.1* with `IO::Buffer` support.
				# @asynchronous May be non-blocking.
				#
				# @parameter io [IO] The IO object to write to.
				# @parameter buffer [IO::Buffer] The buffer to write from.
				# @parameter length [Integer] The minimum number of bytes to write.
				# @parameter offset [Integer] The offset within the buffer to write from.
				def io_write(io, buffer, length, offset = 0)
					fiber = Fiber.current
					
					if timeout = get_timeout(io)
						timer = @timers.after(timeout) do
							fiber.raise(::IO::TimeoutError, "Timeout (#{timeout}s) while waiting for IO to become writable!")
						end
					end
					
					@selector.io_write(fiber, io, buffer, length, offset)
				ensure
					timer&.cancel!
				end
			end
		end
		
		# Used to defer stopping the current task until later.
		class RaiseException
			# Create a new stop later operation.
			#
			# @parameter task [Task] The task to stop later.
			def initialize(fiber, exception)
				@fiber = fiber
				@exception = exception
			end
			
			# @returns [Boolean] Whether the task is alive.
			def alive?
				@fiber.alive?
			end
			
			# Transfer control to the operation - this will stop the task.
			def transfer
				@fiber.raise(@exception)
			end
		end
		
		# Raise an exception on the specified fiber, waking up the event loop if necessary.
		#
		# @public Since *Ruby v3.4.x*.
		# @asynchronous May be called from any thread.
		#
		# @parameter fiber [Fiber] The fiber to raise the exception on.
		# @parameter exception [Exception] The exception to raise.
		def fiber_interrupt(fiber, exception)
			unblock(nil, RaiseException.new(fiber, exception))
		end
		
		# Invoke the block, but after the specified timeout, raise {TimeoutError} in any currenly blocking operation. If the block runs to completion before the timeout occurs or there are no non-blocking operations after the timeout expires, the code will complete without any exception.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May raise an exception at any interruption point (e.g. blocking operations).
		#
		# @parameter duration [Numeric] The time in seconds, in which the task should complete.
		# @parameter exception [Class] The exception class to raise.
		# @parameter message [String] The message to pass to the exception.
		# @yields {|duration| ...} The block to execute with a timeout.
		def timeout_after(duration, exception = TimeoutError, message = "execution expired", &block)
			fiber = Fiber.current
			
			timer = @timers.after(duration) do
				if fiber.alive?
					fiber.raise(exception, message)
				end
			end
			
			yield timer
		ensure
			timer&.cancel!
		end
		
		# Wait for the specified process ID to exit.
		#
		# @public Since *Ruby v3.1*.
		# @asynchronous May be non-blocking.
		#
		# @parameter pid [Integer] The process ID to wait for.
		# @parameter flags [Integer] A bit-mask of flags suitable for `Process::Status.wait`.
		# @returns [Process::Status] A process status instance.
		# @asynchronous May be non-blocking..
		def process_wait(pid, flags)
			return @selector.process_wait(Fiber.current, pid, flags)
		end
		
		# Run one iteration of the event loop.
		#
		# When terminating the event loop, we already know we are finished. So we don't need to check the task tree. This is a logical requirement because `run_once` ignores transient tasks. For example, a single top level transient task is not enough to keep the reactor running, but during termination we must still process it in order to terminate child tasks.
		#
		# @parameter timeout [Float | Nil] The maximum timeout, or if nil, indefinite.
		# @returns [Boolean] Whether there is more work to do.
		private def run_once!(timeout = nil)
			interval = @timers.wait_interval
			
			# If there is no interval to wait (thus no timers), and no tasks, we could be done:
			if interval.nil?
				# Allow the user to specify a maximum interval if we would otherwise be sleeping indefinitely:
				interval = timeout
			elsif interval < 0
				# We have timers ready to fire, don't sleep in the selctor:
				interval = 0
			elsif timeout and interval > timeout
				interval = timeout
			end
			
			begin
				@selector.select(interval)
			rescue Errno::EINTR
				# Ignore.
			end
			
			@timers.fire
			
			# The reactor still has work to do:
			return true
		end
		
		# Run one iteration of the event loop.
		#
		# @public Since *Async v1*.
		# @asynchronous Must be invoked from blocking (root) fiber.
		#
		# @parameter timeout [Float | Nil] The maximum timeout, or if nil, indefinite.
		# @returns [Boolean] Whether there is more work to do.
		def run_once(timeout = nil)
			Kernel.raise "Running scheduler on non-blocking fiber!" unless Fiber.blocking?
			
			if self.ready?
				self.run_once!(timeout)
			else
				return false
			end
		end
		
		# Checks and clears the interrupted state of the scheduler.
		#
		# @returns [Boolean] Whether the reactor has been interrupted.
		private def interrupted?
			if @interrupted
				@interrupted = false
				return true
			end
			
			if Thread.pending_interrupt?
				return true
			end
			
			return false
		end
		
		private def run_loop(&block)
			Thread.handle_interrupt(::SignalException => :never) do
				until self.interrupted?
					# If we are finished, we need to exit:
					break unless yield
				end
			end
		end
		
		# Run the reactor until all tasks are finished. Proxies arguments to {#async} immediately before entering the loop, if a block is provided.
		#
		# Forwards all parameters to {#async} if a block is given.
		#
		# @public Since *Async v1*.
		#
		# @yields {|task| ...} The top level task, if a block is given.
		# @returns [Task] The initial task that was scheduled into the reactor.
		def run(...)
			::Kernel.raise ClosedError if @selector.nil?
			
			initial_fiber = self.fiber(...) if block_given?
			
			self.run_loop do
				self.run_once
			end
			
			return initial_fiber
		end
	end
end
