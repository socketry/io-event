# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		it "can allocate and deallocate multiple times" do
			pipes = 1000.times.collect{IO.pipe}
			
			100000.times do
				selector = subject.new(Fiber.current)
				
				10.times do
					Fiber.new do
						selector.io_wait(Fiber.current, pipes.sample[1], IO::WRITABLE)
					end.transfer
				end
				
				selector.select(0)
			ensure
				selector&.close
			end
			
			GC.start
		end
	end
end
