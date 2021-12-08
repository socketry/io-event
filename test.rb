#!/usr/bin/env ruby

$LOAD_PATH << "ext"

require_relative 'lib/io/event'

require 'fiber'
require 'socket'

require 'io/nonblock'

local, remote = UNIXSocket.pair
selector = IO::Event::Selector::Select.new(Fiber.current)

events = Array.new
sockets = UNIXSocket.pair
local = sockets.first
remote = sockets.last

fiber = Fiber.new do
	events << :wait_readable
	
	selector.io_wait(Fiber.current, local, IO::READABLE)
	
	events << :readable
end

events << :transfer
fiber.transfer

remote.puts "Hello World"

events << :select

selector.select(1)

pp events == [
	:transfer, :wait_readable,
	:select, :readable
]
