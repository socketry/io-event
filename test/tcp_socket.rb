# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"
require "io/event/test_scheduler"

require "socket"
require "io/nonblock"

describe TCPSocket do
	let(:scheduler) {IO::Event::TestScheduler.new}
	
	def make_socket_pair
		server_socket = TCPServer.new("localhost", 0)
		port = server_socket.addr[1]
		
		client = TCPSocket.new("localhost", port)
		server = server_socket.accept
		
		client.nonblock = true
		server.nonblock = true
		
		return server_socket, client, server
	end
	
	it "can wait for readability" do
		server_socket, client, server = make_socket_pair
		
		result = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			result = client.wait_readable
		end
		
		Fiber.schedule do
			server.write("Hello")
			server.flush
		end
		
		scheduler.run
		
		expect(result).to be_truthy
	ensure
		Fiber.set_scheduler(nil)
		client&.close
		server&.close
		server_socket&.close
	end
	
	it "can wait for writability" do
		server_socket, client, server = make_socket_pair
		
		result = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			result = client.wait_writable
		end
		
		scheduler.run
		
		expect(result).to be_truthy
	ensure
		Fiber.set_scheduler(nil)
		client&.close
		server&.close
		server_socket&.close
	end
	
	it "can wait for writability repeatedly" do
		server_socket, client, server = make_socket_pair
		
		results = []
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			3.times do
				results << client.wait_writable
			end
		end
		
		scheduler.run
		
		expect(results.size).to be == 3
		results.each do |result|
			expect(result).to be_truthy
		end
	ensure
		Fiber.set_scheduler(nil)
		client&.close
		server&.close
		server_socket&.close
	end
	
	it "can wait for readability when the peer closes" do
		server_socket, client, server = make_socket_pair
		
		result = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			result = client.wait_readable
		end
		
		Fiber.schedule do
			server.close
		end
		
		scheduler.run
		
		expect(result).to be_truthy
	ensure
		Fiber.set_scheduler(nil)
		client&.close
		server&.close unless server&.closed?
		server_socket&.close
	end
	
	it "can read and write using scheduler hooks" do
		server_socket, client, server = make_socket_pair
		
		result = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			result = client.read(5)
		end
		
		Fiber.schedule do
			server.write("Hello")
			server.flush
		end
		
		scheduler.run
		
		expect(result).to be == "Hello"
	ensure
		Fiber.set_scheduler(nil)
		client&.close
		server&.close
		server_socket&.close
	end
	
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
