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
	end
	
	def io_wait(io, events, timeout)
		fiber = Fiber.current
		@waiting[fiber] = io
		@selector.io_wait(Fiber.current, io, events)
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

Fiber.set_scheduler(Scheduler.new)

port = Integer(ARGV.pop || 9090)

Fiber.schedule do
	server = TCPServer.new('localhost', port)
	
	loop do
		peer, address = server.accept
		
		Fiber.schedule do
			peer.recv(1024)
			peer.send("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n", 0)
			peer.close
		end
	end
end

