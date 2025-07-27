#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.

require "socket"
require "time"

# Assuming request per connection:
RESPONSE = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"

port = Integer(ARGV.pop || 9090)
server = TCPServer.new("localhost", port)

loop do
	peer, address = server.accept
	
	# This is by far the fastest path, clocking in at around 80,000 requests per second in a single thread.
	peer.recv(1024) rescue nil
	peer.send(RESPONSE, 0)
	
	# This drops us to about 1/4 the performance due to the overhead of blocking operations.
	# while (peer.recv(1024) rescue nil)
	# 	peer.send(RESPONSE, 0)
	# end
	
	peer.close
end

