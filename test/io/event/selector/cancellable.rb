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
			skip "Single-transfer io_read does not wait for readiness" if defined?(IO::Buffer::VERSION) && IO::Buffer::VERSION >= 3
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				
				10.times do
					expect{selector.io_read(Fiber.current, input, buffer, 1)}.to raise_exception(Interrupt)
				end
			end
			
			# Enter the `io_read` operation:
			reader.transfer
			
			while reader.alive?
				reader.raise(Interrupt)
				selector.select(0)
			end
		end
		
		it "continues cancellation when interrupted again" do
			skip "Requires the URing selector" unless defined?(IO::Event::Selector::URing) && selector.is_a?(IO::Event::Selector::URing)
			
			buffer = IO::Buffer.new(64)
			error = nil
			
			reader = Fiber.new do
				begin
					if selector.method(:io_read).arity == 5
						selector.io_read(Fiber.current, input, buffer, 0, 1)
					else
						selector.io_read(Fiber.current, input, buffer, 1)
					end
				rescue Interrupt => exception
					error = exception
				end
			end
			
			# Submit the read and leave it pending on the empty pipe:
			reader.transfer
			
			# The first interruption enters cancel_and_wait and yields while the
			# original operation is still outstanding:
			reader.raise(Interrupt)
			expect(reader).to be(:alive?)
			
			# A second interruption must be deferred until cancellation cleanup
			# has consumed the original operation's completion:
			reader.raise(Interrupt)
			continued_cancellation = reader.alive?
			
			if continued_cancellation
				selector.select(0.1) while reader.alive?
			end
			
			expect(continued_cancellation).to be == true
			expect(error).to be_a(Interrupt)
		end
		
		it "can cancel waits" do
			skip "Single-transfer io_read does not wait for readiness" if defined?(IO::Buffer::VERSION) && IO::Buffer::VERSION >= 3
			
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
		
		it "can reuse completions after cancelling waits" do
			skip "Requires the URing selector" unless defined?(IO::Event::Selector::URing) && selector.is_a?(IO::Event::Selector::URing)
			
			pid = Process.fork do
				local_selector = selector.class.new(Fiber.current)
				local_input, local_output = IO.pipe
				
				begin
					cancelled_waiter = Fiber.new do
						local_selector.io_wait(Fiber.current, local_input, IO::READABLE)
					rescue Interrupt
						# The pending wait was cancelled.
					end
					
					cancelled_waiter.transfer
					cancelled_waiter.raise(Interrupt)
					local_selector.select(0.01)
					
					waiters = 2.times.map do
						Fiber.new do
							local_selector.io_wait(Fiber.current, local_input, IO::READABLE)
						rescue Interrupt
							# The pending wait was cancelled.
						end
					end
					
					waiters.each(&:transfer)
				ensure
					local_selector.close
					local_input.close
					local_output.close
				end
			end
			
			_, status = Process.wait2(pid)
			expect(status).to be(:success?)
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
