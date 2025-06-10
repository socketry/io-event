# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event/timers"

module IO::Event
	# A test fiber scheduler that uses WorkerPool for blocking operations.
	# 
	# This scheduler implements the fiber scheduler interface and delegates
	# blocking operations to a WorkerPool instance for testing.
	#
	# @example Testing usage
	# ```ruby
	# # Create with default selector and worker pool
	# scheduler = IO::Event::TestScheduler.new
	# 
	# # Or provide custom selector and/or worker pool
	# selector = IO::Event::Selector.new(Fiber.current)
	# worker_pool = IO::Event::WorkerPool.new(maximum_worker_count: 4)
	# scheduler = IO::Event::TestScheduler.new(selector: selector, worker_pool: worker_pool)
	# 
	# Fiber.set_scheduler(scheduler)
	# 
	# # Standard Ruby operations that use rb_nogvl will be handled by the worker pool
	# # Examples: sleep, file I/O, network operations, etc.
	# Fiber.schedule do
	#   sleep(0.001)  # This triggers rb_nogvl and uses the worker pool
	# end.resume
	# ```
	class TestScheduler
		def initialize(selector: nil, worker_pool: nil, max_threads: 2)
			@selector = selector || ::IO::Event::Selector.new(Fiber.current)
			@worker_pool = worker_pool || WorkerPool.new(maximum_worker_count: max_threads)
			@timers = ::IO::Event::Timers.new

			# Track the number of fibers that are blocked.
			@blocked = 0
		end
		
		# @attribute [WorkerPool] The worker pool used for executing blocking operations.
		attr_reader :worker_pool
		
		# @attribute [IO::Event::Selector] The I/O event selector used for managing fiber scheduling.
		attr_reader :selector

		# Required fiber scheduler hook - delegates to WorkerPool
		def blocking_operation_wait(operation)
			# Submit the operation to the worker pool and wait for completion
			@worker_pool.call(operation)
		end
		
		# Required fiber scheduler hooks
		def close
			@selector&.close
			# WorkerPool doesn't have a close method, just clear the reference
			@worker_pool = nil
		end
		
		def block(blocker, timeout = nil)
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
		
		def unblock(blocker, fiber)
			if selector = @selector
				selector.push(fiber)
				selector.wakeup
			end
		end

		class FiberInterrupt
			def initialize(fiber, exception)
				@fiber = fiber
				@exception = exception
			end

			def alive?
				@fiber.alive?
			end

			def transfer
				@fiber.raise(@exception)
			end
		end

		def fiber_interrupt(fiber, exception)
			unblock(nil, FiberInterrupt.new(fiber, exception))
		end
		
		def io_wait(io, events, timeout = nil)
			fiber = Fiber.current
			
			if timeout
				timer = @timers.after(timeout) do
					fiber.transfer
				end
			end
			
			return @selector.io_wait(fiber, io, events)
		ensure
			timer&.cancel!
		end
		
		def kernel_sleep(duration = nil)
			if duration
				self.block(nil, duration)
			else
				@selector.transfer
			end
		end
		
		def fiber(&block)
			Fiber.new(&block).tap(&:transfer)
		end
		
		# Run the scheduler event loop
		def run
			while @blocked > 0 or @timers.size > 0
				interval = @timers.wait_interval
				@selector.select(interval)
				@timers.fire
			end
		end

		def scheduler_close(error = $!)
			self.run
		ensure
			self.close
		end
		
		private
		
		def transfer
			@selector.transfer
		end
		
		def push(fiber)
			@selector.push(fiber)
		end
		
		def wakeup
			@selector.wakeup
		end
	end
end 