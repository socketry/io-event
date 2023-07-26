#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require 'async'
require 'async/io/tcp_socket'

port = Integer(ARGV.pop || 9090)

Async do |task|
	server = Async::IO::TCPServer.new('localhost', port)
	
	loop do
		peer, address = server.accept
		
		task.async do
			peer.recv(1024)
			peer.send("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
			peer.close
		end
	end
end
