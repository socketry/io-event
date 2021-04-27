#!/usr/bin/env ruby

require_relative 'lib/event'

require 'fiber'
require 'socket'

local, remote = UNIXSocket.pair
selector = Event::Backend::URing.new(Fiber.current)

f1 = Fiber.new do
	buffer = String.new
	length = selector.io_read(Fiber.current, local, buffer, 0, 1024)
	pp ["f1", length, buffer, buffer.bytesize]
end

f2 = Fiber.new do
	buffer = "Hello World"
	length = selector.io_write(Fiber.current, remote, buffer, 0, buffer.bytesize)
	pp ["f2", length, buffer, buffer.bytesize]
end

f1.transfer
f2.transfer

pp ["select", selector.select(1)]

