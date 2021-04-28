#!/usr/bin/env ruby

require 'benchmark/ips'
require 'fiber'

require_relative '../lib/event'

Benchmark.ips do |x|
	input, output = IO.pipe
	
	output.puts "Hello World"
	
	Event::Backend.constants.each do |name|
		x.report(name) do |times|
			i = 0
			
			backend = Event::Backend.const_get(name).new(Fiber.current)
			
			fiber = Fiber.new do
				while true
					backend.io_wait(fiber, input, Event::READABLE)
				end
			end
			
			# Start initial wait:
			fiber.transfer
			
			while i < times
				backend.select(1)
				
				i += 1
			end
		end
	end
	
	# Compare the iterations per second of the various reports!
	x.compare!
end