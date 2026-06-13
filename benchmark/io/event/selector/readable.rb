# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2026, by Samuel Williams.

require "sus/fixtures/benchmark"
require "io/event"

# Measures selector dispatch overhead when many waiting fibers are immediately readable.
#
# Run with: bundle exec sus --verbose benchmark/io/event/selector/readable.rb

FIBER_COUNT = 256

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	next unless klass.respond_to?(:new)
	
	describe "#{klass}" do
		include Sus::Fixtures::Benchmark
		
		def close_pipes(pipes)
			pipes.each do |input, output|
				input.close unless input.closed?
				output.close unless output.closed?
			end
		end
		
		measure "select readable waiters" do |repeats|
			selector = klass.new(Fiber.current)
			pipes = []
			fibers = []
			
			FIBER_COUNT.times do
				input, output = IO.pipe
				output.write("!")
				pipes << [input, output]
				
				fiber = Fiber.new do
					loop do
						selector.io_wait(Fiber.current, input, IO::READABLE)
					end
				rescue RuntimeError
					# Raised during cleanup to stop the benchmark fiber.
				end
				
				fibers << fiber
			end
			
			fibers.each(&:transfer)
			
			repeats.times do
				selector.select(0)
			end
		ensure
			fibers&.each do |fiber|
				fiber.raise("Stop") if fiber.alive?
			rescue RuntimeError
				# The fiber may already be stopped or waiting on a closed pipe.
			end
			
			close_pipes(pipes) if pipes
			selector&.close
		end
	end
end
