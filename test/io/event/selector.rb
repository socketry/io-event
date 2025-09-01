# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.
# Copyright, 2023, by Math Ieu.

require "io/event"
require "io/event/selector"
require "io/event/debug/selector"

require "socket"
require "fiber"

require "unix_socket"

class FakeFiber
	def initialize(alive = true)
		@alive = alive
		@count = 0
	end
	
	attr :count
	
	def alive?
		@alive
	end
	
	def transfer
		@count += 1
	end
end

Selector = Sus::Shared("a selector") do
	with "#select" do
		let(:quantum) {0.2}
		
		it "can select with 0s timeout" do
			expect do
				selector.select(0)
			end.to have_duration(be < quantum)
		end
		
		it "can select with a short timeout" do
			expect do
				selector.select(0.01)
			end.to have_duration(be <= (0.01 + quantum))
		end
		
		it "raises an error when given an invalid duration" do
			expect do
				selector.select("invalid")
			end.to raise_exception
		end
	end
	
	with "#idle_duration" do
		it "can report idle duration" do
			10.times do
				selector.select(0.001)
				expect(selector.idle_duration).to be > 0.0
				
				selector.select(0)
				expect(selector.idle_duration).to be == 0.0
			end
		end
	end
	
	with "#wakeup" do
		it "can wakeup selector from different thread" do
			thread = Thread.new do
				sleep 0.001
				selector.wakeup
			end
			
			expect do
				selector.select(1)
			end.to have_duration(be < 1)
		ensure
			thread.join
		end
		
		it "can wakeup selector from different thread twice in a row" do
			2.times do
				thread = Thread.new do
					sleep 0.001
					selector.wakeup
				end
				
				expect do
					selector.select(1)
				end.to have_duration(be < 1)
			ensure
				thread.join
			end
		end
		
		it "ignores wakeup if not selecting" do
			expect(selector.wakeup).to be == false
		end
		
		it "doesn't block when readying another fiber" do
			fiber = FakeFiber.new
			
			10.times do |i|
				thread = Thread.new do
					sleep(i / 10000.0)
					selector.push(fiber)
					selector.wakeup
				end
				
				expect do
					selector.select(1.0)
				end.to have_duration(be < 1.0)
			ensure
				thread.join
			end
		end
	end
	
	with "#io_wait" do
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		it "can wait for an io to become readable" do
			fiber = Fiber.new do
				events << :wait_readable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::READABLE)
				).to be == IO::READABLE
				
				events << :readable
			end
			
			events << :transfer
			fiber.transfer
			
			remote.puts "Hello World"
			
			events << :select
			
			selector.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable,
				:select, :readable
			]
		end
		
		it "can wait for an io to become writable" do
			fiber = Fiber.new do
				events << :wait_writable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::WRITABLE)
				).to be == IO::WRITABLE
				
				events << :writable
			end
			
			events << :transfer
			fiber.transfer
			
			events << :select
			selector.select(1)
			
			expect(events).to be == [
				:transfer, :wait_writable,
				:select, :writable
			]
		end
		
		it "can read and write from two different fibers" do
			readable = writable = false
			
			read_fiber = Fiber.new do
				events << :wait_readable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::READABLE)
				).to be == IO::READABLE
				
				readable = true
			end
			
			write_fiber = Fiber.new do
				events << :wait_writable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::WRITABLE)
				).to be == IO::WRITABLE
				
				writable = true
			end
			
			events << :transfer
			read_fiber.transfer
			write_fiber.transfer
			
			remote.puts "Hello World"
			events << :select
			selector.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable, :wait_writable,
				:select
			]
			
			expect(readable).to be == true
			expect(writable).to be == true
		end
		
		it "can read and write from two different fibers (alternate)" do
			read_fiber = Fiber.new do
				events << :wait_readable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::READABLE)
				).to be == IO::READABLE
				
				events << :readable
			end
			
			write_fiber = Fiber.new do
				events << :wait_writable
				
				expect(
					selector.io_wait(Fiber.current, local, IO::WRITABLE)
				).to be == IO::WRITABLE
				
				events << :writable
			end
			
			events << :transfer
			read_fiber.transfer
			write_fiber.transfer
			
			events << :select1
			selector.select(1)
			remote.puts "Hello World"
			events << :select2
			selector.select(1)
			
			expect(events).to be == [
				:transfer,
				:wait_readable,
				:wait_writable,
				:select1,
				:writable,
				:select2,
				:readable,
			]
		end
		
		it "can wait consecutively on two different io objects that share the same file descriptor" do
			fiber = Fiber.new do
				events << :write1
				remote.puts "Hello World"
				
				events << :wait_readable1
				
				expect(
					selector.io_wait(Fiber.current, local, IO::READABLE)
				).to be == IO::READABLE
				
				events << :readable1
				
				events << :new_io
				fileno = local.fileno
				local.close
				
				new_local, new_remote = UNIXSocket.pair
				
				# Make sure we attempt to wait on the same file descriptor:
				if new_remote.fileno == fileno
					new_local, new_remote = new_remote, new_local
				end
				
				if new_local.fileno != fileno
					warn "Could not create new IO object with same FD, test ineffective!"
				end
				
				events << :write2
				new_remote.puts "Hello World"
				
				events << :wait_readable2
				
				expect(
					selector.io_wait(Fiber.current, new_local, IO::READABLE)
				).to be == IO::READABLE
				
				events << :readable2
			end
			
			events << :transfer
			fiber.transfer
			
			events << :select1
			
			selector.select(1)
			
			events << :select2
			
			selector.select(1)
			
			expect(events).to be == [
				:transfer,
				:write1,
				:wait_readable1,
				:select1,
				:readable1,
				:new_io,
				:write2,
				:wait_readable2,
				:select2,
				:readable2,
			]
		end
		
		it "can handle exception during wait" do
			fiber = Fiber.new do
				events << :wait_readable
				
				expect do
					while true
						selector.io_wait(Fiber.current, local, IO::READABLE)
						events << :readable
					end
				end.to raise_exception(RuntimeError, message: be =~ /Boom/)
				
				events << :error
			end
			
			events << :transfer
			fiber.transfer
			
			events << :select
			selector.select(0)
			fiber.raise(RuntimeError.new("Boom"))
			
			events << :puts
			remote.puts "Hello World"
			selector.select(0)
			
			expect(events).to be == [
				:transfer, :wait_readable,
				:select, :error, :puts
			]
		end
		
		it "can have two fibers reading from the same io" do
			fiber1 = Fiber.new do
				events << :wait_readable1
				selector.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable
			rescue
				events << :error1
			end
			
			fiber2 = Fiber.new do
				events << :wait_readable2
				selector.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable
			rescue
				events << :error2
			end
			
			events << :transfer
			fiber1.transfer
			fiber2.transfer
			
			remote.puts "Hello World"
			events << :select
			selector.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable1, :wait_readable2,
				:select, :readable, :readable
			]
		end
		
		it "can handle exception raised during wait from another fiber that was waiting on the same io" do
			[false, true].each do |swapped| # Try both orderings.
				writable1 = writable2 = false
				error1 = false
				raised1 = false
				
				boom = Class.new(RuntimeError)
				
				fiber1 = fiber2 = nil
				
				fiber1 = Fiber.new do
					begin
						selector.io_wait(Fiber.current, local, IO::WRITABLE)
					rescue boom
						error1 = true
						# Transfer back to the signaling fiber to simulate doing something similar to raising an exception in an asynchronous task or thread.
						fiber2.transfer
					end
					writable1 = true
				end
				
				fiber2 = Fiber.new do
					selector.io_wait(Fiber.current, local, IO::WRITABLE)
					# Don't do anything if the other fiber was resumed before we were by the selector.
					unless writable1
						raised1 = true
						fiber1.raise(boom) # Will return here.
					end
					writable2 = true
				end
				
				fiber1.transfer unless swapped
				fiber2.transfer
				fiber1.transfer if swapped
				selector.select(0)
				
				# If fiber2 did manage to be resumed by the selector before fiber1, it should have raised an exception in fiber1, and fiber1 should not have been resumed by the selector since its #io_wait call should have been cancelled.
				expect(error1).to be == raised1
				expect(writable1).to be == !raised1
				expect(writable2).to be == true
			end
		end
		
		it "can handle io being closed by another fiber while waiting" do
			error = nil
			
			wait_fiber = Fiber.new do
				wait_fiber_started = true
				events << :wait_readable
				begin
					result = selector.io_wait(Fiber.current, local, IO::READABLE)
					$stderr.puts "result: #{result}"
					events << :readable
				rescue => error
					# This isn't a reliable state transition.
					# events << :error
				end
			end
			
			close_fiber = Fiber.new do
				events << :close_io
				local.close
			end
			
			events << :transfer
			wait_fiber.transfer
			close_fiber.transfer
			
			expect do
				events << :select
				selector.select(0)
			end.not.to raise_exception
			
			expect(events).to be == [
				:transfer, :wait_readable, :close_io, :select
			]
			
			# io_uring will raise an EBADF error if the IO is closed while waiting.
			# But other selectors are not capable of detecting this.
			# expect(error).to be_nil
		end
	end
	
	with "#io_read" do
		let(:message) {"Hello World"}
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		let(:buffer) {IO::Buffer.new(1024, IO::Buffer::MAPPED)}
		
		it "can read a single message" do
			return unless selector.respond_to?(:io_read)
			
			fiber = Fiber.new do
				events << :io_read
				offset = selector.io_read(Fiber.current, local, buffer, message.bytesize)
				expect(buffer.get_string(0, offset)).to be == message
			end
			
			fiber.transfer
			
			events << :write
			remote.write(message)
			
			selector.select(1)
			
			expect(events).to be == [
				:io_read, :write
			]
		end
		
		it "can handle partial reads" do
			return unless selector.respond_to?(:io_read)
			
			fiber = Fiber.new do
				events << :io_read
				offset = selector.io_read(Fiber.current, local, buffer, message.bytesize)
				expect(buffer.get_string(0, offset)).to be == message
			end
			
			fiber.transfer
			
			events << :write
			remote.write(message[0...5])
			selector.select(1)
			remote.write(message[5...message.bytesize])
			selector.select(1)
			
			expect(events).to be == [
				:io_read, :write
			]
		end
		
		it "can stop reading when reads are ready" do
			# This could trigger a busy-loop in the KQueue selector.
			return unless selector.respond_to?(:io_read)
			
			fiber = Fiber.new do
				offset = selector.io_read(Fiber.current, local, buffer, message.bytesize)
				expect(buffer.get_string(0, offset)).to be == message
				sleep(0.001)
			end
			
			fiber.transfer
			
			remote.write(message)
			
			expect(selector.select(0)).to be == 1
			
			remote.write(message)
			
			result = nil
			3.times do
				result = selector.select(0)
				break if result == 0
			end
			
			expect(result).to be == 0
		end
	end
	
	with "#io_write" do
		let(:message) {"Hello World"}
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		it "can write a single message" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			return unless selector.respond_to?(:io_write)
			
			fiber = Fiber.new do
				events << :io_write
				buffer = IO::Buffer.for(message.dup)
				result = selector.io_write(Fiber.current, local, buffer, buffer.size)
				expect(result).to be == message.bytesize
				local.close
			end
			
			fiber.transfer
			
			selector.select(0)
			
			events << :read
			expect(remote.read).to be == message
			
			expect(events).to be == [
				:io_write, :read
			]
		end
	end
	
	with "#process_wait" do
		it "can wait for a process which has terminated already" do
			result = nil
			events = []
			
			fiber = Fiber.new do
				pid = Process.spawn("true")
				result = selector.process_wait(Fiber.current, pid, 0)
				expect(result).to be(:success?)
				events << :process_finished
			end
			
			fiber.transfer
			
			while fiber.alive?
				selector.select(1)
			end
			
			expect(events).to be == [:process_finished]
			expect(result.success?).to be == true
		end
		
		it "can wait for a process to terminate" do
			result = nil
			events = []
			
			fiber = Fiber.new do
				pid = Process.spawn("sleep 0.001")
				result = selector.process_wait(Fiber.current, pid, 0)
				expect(result).to be(:success?)
				events << :process_finished
			end
			
			fiber.transfer
			
			while fiber.alive?
				selector.select(0)
			end
			
			expect(events).to be == [:process_finished]
			expect(result).to be(:success?)
		end
		
		it "can wait for two processes sequentially" do
			result1 = result2 = nil
			events = []
			
			fiber = Fiber.new do
				pid1 = Process.spawn("sleep 0")
				pid2 = Process.spawn("sleep 0")
				
				result1 = selector.process_wait(Fiber.current, pid1, 0)
				events << :process_finished1
				
				result2 = selector.process_wait(Fiber.current, pid2, 0)
				events << :process_finished2
			end
			
			fiber.transfer
			
			while fiber.alive?
				selector.select(0)
			end
			
			expect(events).to be == [:process_finished1, :process_finished2]
			expect(result1).to be(:success?)
			expect(result2).to be(:success?)
		end
	end
	
	with "#resume" do
		it "can resume a fiber" do
			other_fiber_count = 0
			
			5.times do
				fiber = Fiber.new do
					other_fiber_count += 1
				end
				
				selector.resume(fiber)
			end
			
			expect(other_fiber_count).to be == 5
		end
	end
end

describe IO::Event::Selector do
	with ".default" do
		it "can get the default selector" do
			expect(subject.default).to be_a(Module)
		end
		
		it "returns the default if an invalid name is provided" do
			env = {"IO_EVENT_SELECTOR" => "invalid"}
			expect{subject.default(env)}.to raise_exception(NameError)
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		with ".default" do
			it "can get the specified selector" do
				env = {"IO_EVENT_SELECTOR" => name}
				expect(IO::Event::Selector.default(env)).to be == klass
			end
		end
		
		with ".new" do
			let(:count) {8}
			let(:loop) {Fiber.current}
			
			it "can create multiple selectors" do
				selectors = count.times.map do |i|
					subject.new(loop)
				end
				
				expect(selectors.size).to be == count
				
				selectors.each(&:close)
			end
		end
		
		with "an instance" do
			before do
				@loop = Fiber.current
				@selector = subject.new(@loop)
			end
			
			after do
				@selector&.close
			end
			
			attr :loop
			attr :selector
			
			it_behaves_like Selector
		end
	end
end

describe IO::Event::Debug::Selector do
	before do
		@loop = Fiber.current
		@selector = subject.new(IO::Event::Selector.new(loop))
	end
	
	after do
		@selector&.close
	end
	
	attr :loop
	attr :selector
	
	it_behaves_like Selector
end
