#!/usr/bin/env ruby
# frozen_string_literal: true

require_relative 'scheduler'

scheduler = Scheduler.new
Fiber.set_scheduler(scheduler)

port = Integer(ARGV.pop || 9090)

RESPONSE = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

Fiber.schedule do
	server = TCPServer.new('localhost', port)
	
	loop do
		peer, address = server.accept
		
		peer.readpartial(1024) rescue nil
		peer.write(RESPONSE)
		peer.close
	end
end

