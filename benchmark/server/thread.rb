#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'socket'

port = Integer(ARGV.pop || 9090)
server = TCPServer.new('localhost', port)

loop do
	peer = server.accept
	
	Thread.new do
		peer.recv(1024)
		peer.send("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n", 0)
		peer.close
	end
end
