#!/usr/bin/env ruby

require 'benchmark/ips'
require 'fiber'
require 'console'

require_relative '../lib/event'

Event::Selector.constants.each do |name|
	selector = Event::Selector.const_get(name).new(Fiber.current)
	
	fibers = 256.times.map do |index|
		input, output = IO.pipe
		output.puts "Hello World"
		
		fiber = Fiber.new do
			while true
				selector.io_wait(fiber, input, Event::READABLE)
			end
		rescue RuntimeError
			# Ignore.
		ensure
			input.close
			output.close
		end
	end
	
	# Start initial wait:
	fibers.each(&:transfer)
	
	Console.logger.measure(selector) do
		i = 10_000
		while (i -= 1) > 0
			selector.select(0)
		end
	end
	
	fibers.each{|fiber| fiber.raise("Stop")}
end
