# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.

$LOAD_PATH << File.expand_path("../../lib", __dir__)
$LOAD_PATH << File.expand_path("../../ext", __dir__)

require "io/event"

require "socket"
require "fiber"

class Scheduler
	def initialize(selector = nil)
		@fiber = Fiber.current
		@selector = selector || IO::Event::Selector.new(@fiber)
		@waiting = 0
		
		unless @selector.respond_to?(:io_close)
			instance_eval{undef io_close}
		end
		
		@mutex = Mutex.new
	end
	
	def block(blocker, timeout)
		raise NotImplementedError
	end
	
	def unblock(blocker, fiber)
		raise NotImplementedError
	end
	
	def io_wait(io, events, timeout)
		fiber = Fiber.current
		@waiting += 1
		@selector.io_wait(fiber, io, events)
	ensure
		@waiting -= 1
	end
	
	def io_close(io)
		@selector.io_close(io)
	end
	
	def kernel_sleep(duration)
		@selector.defer
	end
	
	def close
		while @selector.ready? || @waiting > 0
			begin
				@selector.select(nil)
			rescue Errno::EINTR
				# Ignore.
			end
		end
	rescue Interrupt
		# Exit.
	end
	
	def fiber(&block)
		fiber = Fiber.new(&block)
		
		@selector.resume(fiber)
		
		return fiber
	end
end

class DirectScheduler < Scheduler
	def io_read(io, buffer, length)
		fiber = Fiber.current
		@waiting += 1
		result = @selector.io_read(fiber, io, buffer, length)
	ensure
		@waiting -= 1
	end
	
	def io_write(io, buffer, length)
		fiber = Fiber.current
		@waiting += 1
		@selector.io_write(fiber, io, buffer, length)
	ensure
		@waiting -= 1
	end
end
