# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	before do
		@selector = klass.new(Fiber.current)
		
		# Force the selector to be old generation:
		if Process.respond_to?(:warmup)
			Process.warmup
		else
			3.times{GC.start}
		end
	end
	
	after do
		@selector&.close
	end
	
	describe(klass, unique: name) do
		it "can allocate and deallocate multiple times" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			pipes = 10.times.collect{IO.pipe}
			
			# This test can hang if write barriers are incorrectly implemented.
			
			100000.times do |i|
				Fiber.new do
					@selector.io_wait(Fiber.current, pipes.sample[0], IO::READABLE)
				end.transfer
			end
		end
	end
end
