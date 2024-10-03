#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"
require "fiber"
require "benchmark"

Thread.report_on_exception = true

Benchmark.bmbm do |benchmark|
	benchmark.report("interrupt") do
		count = 0
		input, output = IO.pipe
		
		thread = Thread.new do
			selector = Event::Selector.new(Fiber.current)
			
			while true
				begin
					selector.select(10)
				rescue Errno::EINTR
					# Ignore
					# $stderr.puts "Errno::EINTR"
				end
				
				count += 1
			end
		end
		
		100.times do
			thread.wakeup
			# Thread.pass
		end
		
		pp count: count
	end
	
	benchmark.report("io") do
		count = 0
		input, output = IO.pipe
		
		thread = Thread.new do
			selector = Event::Selector.new(Fiber.current)
			
			fiber = Fiber.new do
				while true
					selector.io_wait(Fiber.current, input, IO::READABLE)
					input.read(1)
				end
			end
			
			fiber.transfer
			
			while true
				selector.select(10)
				count += 1
			end
		end
		
		100.times do
			output.write(".")
			output.flush
		end
		
		pp count: count
	end
end
