# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/selector"

require "socket"

describe IO::Event::Selector::Select do
	before do
		@selector = subject.new(Fiber.current)
	end
	
	after do
		@selector&.close
	end
	
	attr :selector
	
	with "#ready?" do
		it "reports whether a fiber is ready" do
			fiber = Fiber.new{}
			
			expect(selector).not.to be(:ready?)
			
			selector.push(fiber)
			
			expect(selector).to be(:ready?)
			
			selector.select(0)
			
			expect(selector).not.to be(:ready?)
		end
	end
	
	with "Waiter" do
		it "can report whether the fiber is alive" do
			fiber = Fiber.new{}
			waiter = subject::Waiter.new(fiber, IO::READABLE, nil)
			
			expect(waiter).to be(:alive?)
			
			fiber.transfer
			
			expect(waiter).not.to be(:alive?)
		end
		
		it "clears dead fibers while dispatching" do
			fiber = Fiber.new{}
			fiber.transfer
			
			waiter = subject::Waiter.new(fiber, IO::READABLE, nil)
			waiter.dispatch(IO::READABLE){}
			
			expect(waiter.fiber).to be_nil
		end
	end
	
	with "#io_select" do
		it "delegates to IO.select from a worker thread" do
			input, output = IO.pipe
			output.write(".")
			
			readable, = selector.io_select([input], nil, nil, 0)
			
			expect(readable).to be == [input]
		ensure
			input&.close
			output&.close
		end
	end
	
	with "#io_read" do
		it "returns zero at EOF" do
			input, output = IO.pipe
			output.close
			
			buffer = IO::Buffer.new(64)
			
			if defined?(IO::Buffer::VERSION) && IO::Buffer::VERSION >= 3
				expect(selector.io_read(Fiber.current, input, buffer, 0, 1)).to be == 0
			else
				expect(selector.io_read(Fiber.current, input, buffer, 1)).to be == 0
			end
		ensure
			input&.close
		end
	end
	
	with "#io_write (legacy IO::Buffer)" do
		before do
			if defined?(IO::Buffer::VERSION) && IO::Buffer::VERSION >= 3
				skip "Requires the legacy minimum-length write loop"
			end
		end
		
		it "advances the offset after a partial write until the minimum is reached" do
			input, output = IO.pipe
			buffer = IO::Buffer.new(8)
			writes = []
			results = [2, 2]
			
			# Force short writes without depending on the pipe's capacity:
			mock(buffer) do |mock|
				mock.replace(:write) do |io, length, offset|
					writes << [io, length, offset]
					results.shift or raise "Unexpected write!"
				end
			end
			
			expect(selector.io_write(Fiber.current, output, buffer, 4, 1)).to be == 4
			expect(writes).to be == [[output, 0, 1], [output, 0, 3]]
		ensure
			input&.close
			output&.close
			buffer&.free
		end
		
		it "stops on a zero-byte write and returns the bytes already written" do
			input, output = IO.pipe
			buffer = IO::Buffer.new(8)
			writes = []
			results = [2, 0]
			
			mock(buffer) do |mock|
				mock.replace(:write) do |io, length, offset|
					writes << [io, length, offset]
					results.shift or raise "Unexpected write!"
				end
			end
			
			expect(selector.io_write(Fiber.current, output, buffer, 4, 1)).to be == 2
			expect(writes).to be == [[output, 0, 1], [output, 0, 3]]
		ensure
			input&.close
			output&.close
			buffer&.free
		end
	end
	
	with "#select" do
		it "dispatches priority events" do
			server = TCPServer.new("127.0.0.1", 0)
			client = TCPSocket.new("127.0.0.1", server.addr[1])
			socket = server.accept
			events = nil
			
			fiber = Fiber.new do
				events = selector.io_wait(Fiber.current, socket, IO::PRIORITY)
			end
			
			fiber.transfer
			client.send("!", Socket::MSG_OOB)
			
			expect(selector.select(1)).to be == 1
			expect(events).to be == IO::PRIORITY
		ensure
			socket&.close
			client&.close
			server&.close
		end
	end
end
