#!/usr/bin/env ruby

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