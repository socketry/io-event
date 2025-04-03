#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "socket"

RESPONSE = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

port = Integer(ARGV.pop || 9090)
server = TCPServer.new("localhost", port)

loop do
	peer = server.accept
	
	Thread.new do
		peer.recv(1024)
		peer.send(RESPONSE, 0)
		peer.close
	end
end
