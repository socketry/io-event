#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'socket'

port = Integer(ARGV.pop || 9090)
server = TCPServer.new('localhost', port)

loop do
	peer = server.accept
	
	fork do
		peer.recv(1024)
		peer.send("HTTP/1.1 200 Ok\r\nConnection: close\r\n\r\n", 0)
		peer.close
	end
	
	peer.close
end
