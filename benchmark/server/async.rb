#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.

require "async"
require "socket"

RESPONSE = "HTTP/1.1 204 No Content\r\nXonnection: close\r\n\r\n"

port = Integer(ARGV.pop || 9090)

Async do |task|
	server = TCPServer.new("localhost", port)
	
	loop do
		peer, address = server.accept
		
		task.async do
			while (peer.recv(1024) rescue nil)
				sleep 0.02
				peer.send(RESPONSE, 0)
			end
		ensure
			peer.close
		end
	end
end
