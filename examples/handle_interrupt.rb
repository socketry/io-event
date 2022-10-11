#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'io/event'
require 'fiber'
require 'benchmark'

count = 0

thread = Thread.new do
	input, output = IO.pipe
	
	selector = Event::Selector.new(Fiber.current)
	
	fiber = Fiber.new do
		while true
			selector.io_wait(Fiber.current, input, IO::READABLE)
			input.read(1)
		end
	end
	
	fiber.transfer
	
	Thread.handle_interrupt(Interrupt => :never) do
		while true
			$stderr.puts "Selecting"
			begin
				selector.select(1)
			rescue Errno::EINTR
				# Ignore.
			end
			
			Fiber.new do
				sleep 5
			end.resume
			
			# sleep 1
			count += 1
			
			begin
				Thread.handle_interrupt(Interrupt => :immediate) {}
			rescue Interrupt
				$stderr.puts "Interrupted"
			end
		end
	end
end

sleep 2

10.times do
	sleep 1
	$stderr.puts "Sending interrupt"
	thread.raise(Interrupt)
end

sleep 1000

thread.join
