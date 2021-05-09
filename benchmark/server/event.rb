#!/usr/bin/env ruby

require_relative '../../lib/event'
require 'socket'
require 'fiber'

class Scheduler
	def initialize(selector = nil)
		@selector = selector || Event::Backend.new(Fiber.current)
		@ready = []
		@waiting = {}
	end
	
	def io_wait(io, events, timeout)
		fiber = Fiber.current
		@waiting[fiber] = io
		@selector.io_wait(Fiber.current, io, events)
	ensure
		@waiting.delete(fiber)
	end
	
	def close
		while @ready.any? || @waiting.any?
			while fiber = @ready.pop
				fiber.transfer
			end
			
			@selector.select(nil)
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

