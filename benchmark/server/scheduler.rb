
require_relative '../../lib/event'
require 'socket'
require 'fiber'

class Scheduler
	def initialize(selector = nil)
		@fiber = Fiber.current
		@selector = selector || Event::Selector.new(@fiber)
		@pending = []
		@waiting = {}
		
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
		@waiting[fiber] = io
		@selector.io_wait(fiber, io, events)
	ensure
		@waiting.delete(fiber)
	end

	def io_close(io)
		@selector.io_close(io)
	end
	
	def kernel_sleep(duration)
		@selector.defer
	end
	
	def close
		while @selector.ready? || @waiting.any?
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
		
		@selector.transfer(fiber)
		
		return fiber
	end
end

class DirectScheduler < Scheduler
	def io_read(io, buffer, length)
		fiber = Fiber.current
		@waiting[fiber] = io
		result = @selector.io_read(fiber, io, buffer, length)
	ensure
		@waiting.delete(fiber)
	end

	def io_write(io, buffer, length)
		fiber = Fiber.current
		@waiting[fiber] = io
		@selector.io_write(fiber, io, buffer, length)
	ensure
		@waiting.delete(fiber)
	end
end
