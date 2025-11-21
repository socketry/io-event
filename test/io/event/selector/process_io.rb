# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022-2024, by Samuel Williams.
# Copyright, 2023, by Math Ieu.

require "io/event"
require "io/event/selector"
require "io/event/debug/selector"

require "socket"
require "fiber"

ProcessIO = Sus::Shared("process io") do
	it "can wait for a process which has terminated already" do
		result = nil
		
		fiber = Fiber.new do
			input, output = IO.pipe
			
			# For some reason, sleep 0.1 here is very unreliable...?
			pid = Process.spawn("true", out: output)
			output.close
			
			# Internally, this should generate POLLHUP, which is what we want to test:
			expect(selector.io_wait(Fiber.current, input, IO::READABLE)).to be == IO::READABLE
			input.close
			
			_, result = Process.wait2(pid)
		end
		
		fiber.transfer
		
		# Wait until the result is collected:
		until result
			selector.select(1)
		end
		
		expect(result.success?).to be == true
	end

	it "can wait for a process which has terminated already" do
		result = nil
		
		fiber = Fiber.new do
			input, output = IO.pipe
			
			# For some reason, sleep 0.1 here is very unreliable...?
			pid = Process.spawn("true", out: output)
			output.close
			
			# Internally, this should generate POLLHUP, which is what we want to test:
			expect(selector.io_wait(Fiber.current, input, IO::READABLE)).to be == IO::READABLE
			input.close
			
			_, result = Process.wait2(pid)
		end
		
		fiber.transfer
		
		# Wait until the result is collected:
		until result
			selector.select(1)
		end
		
		expect(result.success?).to be == true
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		before do
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		after do
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like ProcessIO
	end
end
