#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require_relative "scheduler"
require "io/nonblock"

RESPONSE = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

#scheduler = DirectScheduler.new
scheduler = Scheduler.new
Fiber.set_scheduler(scheduler)

port = Integer(ARGV.pop || 9090)

Fiber.schedule do
	server = TCPServer.new("localhost", port)
	server.listen(Socket::SOMAXCONN)
	
	loop do
		peer, address = server.accept
		
		Fiber.schedule do
			peer.recv(1024)
			peer.send(RESPONSE, 0)
			peer.close
		end
	end
end

