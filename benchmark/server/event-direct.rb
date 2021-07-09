#!/usr/bin/env ruby

require_relative '../../lib/event'
require 'socket'
require 'fiber'

class Scheduler
	def initialize(selector = nil)
		@fiber = Fiber.current
		@selector = selector || Event::Backend.new(@fiber)
		@ready = []
		@pending = []
		@waiting = {}

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

	def kernel_sleep(duration)
		@ready << Fiber.current
		@fiber.transfer
	end

	def close
		while @ready.any? || @waiting.any?
			@pending, @ready = @ready, @pending
			while fiber = @pending.pop
				fiber.transfer
			end

			@selector.select(@ready.any? ? 0 : nil)
		end
	end
	
	def fiber(&block)
		fiber = Fiber.new(&block)
		
		@ready << Fiber.current
		fiber.transfer
		
		return fiber
	end
end

scheduler = Scheduler.new
Fiber.set_scheduler(scheduler)

port = Integer(ARGV.pop || 9090)

RESPONSE = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

Fiber.schedule do
	server = TCPServer.new('localhost', port)
	
	loop do
		peer, address = server.accept
		
		Fiber.schedule do
			peer.gets
			peer.write(RESPONSE)
			peer.close
		end
	end
end

