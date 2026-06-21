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
			
			expect(selector.io_read(Fiber.current, input, buffer, 1)).to be == 0
		ensure
			input&.close
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
