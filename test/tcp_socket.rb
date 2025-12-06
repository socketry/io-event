# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"
require "io/event/test_scheduler"

require "socket"
require "io/nonblock"

describe TCPSocket do
	let(:scheduler) {IO::Event::TestScheduler.new}
	
	it "can read and write data" do
		chunk_size = 1024*6
		buffer_size = 1024*64
		
		server_socket = TCPServer.new("localhost", 0)
		port = server_socket.addr[1]
		
		client = TCPSocket.new("localhost", port)
		client.nonblock = true
		server = server_socket.accept
		server.nonblock = true
		
		Fiber.set_scheduler(scheduler)
		
		writers = Thread::Queue.new
		2.times do |i|
			Fiber.schedule do
				buffer = i.to_s * chunk_size
				
				128.times do
					server.write(buffer)
					server.flush
				end
				
				writers << :done
			end
		end
		
		Fiber.schedule do
			2.times do
				writers.pop
			end
			
			server.close
		end
		
		Fiber.schedule do
			while result = client.read_nonblock(buffer_size, exception: false)
				case result
				when :wait_readable
					client.wait_readable
				when :wait_writable
					client.wait_writable
				else
					# Done.
				end
			end
		end
		
		scheduler.run
	ensure
		Fiber.set_scheduler(nil)
	end
end
