#!/usr/bin/env ruby

require 'libev_scheduler'
require 'fiber'
require 'socket'

scheduler = Libev::Scheduler.new
Fiber.set_scheduler scheduler

port = Integer(ARGV.pop || 9090)
server = TCPServer.new('localhost', port)

Fiber.schedule do
  loop do
    peer = server.accept

    Fiber.schedule do
      peer.recv(1024)
      peer.send("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n", 0)
      peer.close
    end
  end
end

scheduler.run