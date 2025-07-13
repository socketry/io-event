# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2023-2024, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

require "unix_socket"

Cancellable = Sus::Shared("cancellable") do
	with "a pipe" do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		after do
			input.close
			output.close
		end
		
		it "can cancel reads" do
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				
				10.times do
					expect{selector.io_read(Fiber.current, input, buffer, 1)}.to raise_exception(Interrupt)
				end
			end
			
			# Enter the `io_read` operation:
			reader.transfer
			
			while reader.alive?
				$stderr.puts "Sending interrupt to fiber: #{reader}", reader.backtrace
				reader.raise(Interrupt)
				selector.select(0)
			end
		end
		
		it "can cancel waits" do
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				
				10.times do
					expect{selector.io_wait(Fiber.current, input, IO::READABLE)}.to raise_exception(Interrupt)
					selector.io_read(Fiber.current, input, buffer, 1)
				end
			end
			
			# Enter the `io_read` operation:
			reader.transfer
			
			while reader.alive?
				reader.raise(Interrupt)
				output.write(".")
				selector.select(0.1)
			end
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	# Don't run the test if the selector doesn't support `io_read`/`io_write`:
	next unless klass.instance_methods.include?(:io_read)
	
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
		
		it_behaves_like Cancellable
	end
end
