# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"

module IO::Event
	class TestScheduler
		def initialize(max_threads: 4)
			@worker_pool = IO::Event::WorkerPool.new(max_threads: max_threads)
		end
		
		attr_reader :worker_pool
		
		def fiber(&block)
			Fiber.new(blocking: false, &block).tap(&:resume)
		end
		
		def blocking_operation_wait(blocking_operation)
			@worker_pool.call(blocking_operation)
		end
		
		def close
			# Close worker pool if needed
		end
		
		def run
			# Simple run implementation
		end
		
		# Placeholder implementations for required scheduler methods
		def io_wait(io, events, timeout)
			# Simple blocking implementation for testing
			io.wait_readable if events & IO::READABLE != 0
			io.wait_writable if events & IO::WRITABLE != 0
		end
		
		def kernel_sleep(duration)
			# Simple blocking implementation
			sleep(duration) if duration
		end
		
		def block(blocker, timeout = nil)
			# Simple blocking implementation
			true
		end
		
		def unblock(blocker, fiber)
			# Simple unblock implementation
			fiber.resume if fiber.alive?
		end
	end
end 