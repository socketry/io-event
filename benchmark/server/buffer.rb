#!/usr/bin/env ruby
# frozen_string_literal: true

require_relative 'scheduler'

scheduler = DirectScheduler.new
Fiber.set_scheduler(scheduler)

port = Integer(ARGV.pop || 9090)

RESPONSE_STRING = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

REQUEST = IO::Buffer.new(1024)
RESPONSE = IO::Buffer.new(128)

RESPONSE_SIZE = RESPONSE.copy(RESPONSE_STRING, 0)

Fiber.schedule do
	server = TCPServer.new('localhost', port)

	loop do
		peer, address = server.accept
		
		scheduler.io_read(peer, REQUEST, 1)
		scheduler.io_write(peer, RESPONSE, RESPONSE_SIZE)
		
		peer.close
	end
end

