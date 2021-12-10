# Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, selector to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

require_relative '../../environment'

require 'io/event'
require 'io/event/selector'
require 'io/event/debug/selector'

require 'socket'
require 'fiber'

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
	with '.select' do
		let(:quantum) {0.2}
		
		it "can select with 0s timeout" do
			expect do
				selector.select(0)
			end.to have_duration(be < quantum)
		end
		
		it "can select with 1s timeout" do
			expect do
				selector.select(1)
			end.to have_duration(be_within(quantum).of(1.0))
		end
	end
	
	with '#wakeup' do
		it "can wakeup selector from different thread" do
			thread = Thread.new do
				sleep 0.1
				selector.wakeup
			end
			
			expect do
				selector.select(0.5)
			end.to have_duration(be < 0.5)
		ensure
			thread.join
		end
		
		it "ignores wakeup if not selecting" do
			selector.wakeup
			
			expect do
				selector.select(0.2)
			end.to have_duration(be >= 0.2)
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
	
	with '#io_wait' do
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
		
		it "can handle exception during wait" do
			fiber = Fiber.new do
				events << :wait_readable
				
				expect do
					while true
						selector.io_wait(Fiber.current, local, IO::READABLE)
						events << :readable
					end
				end.to raise_exception(RuntimeError, message: /Boom/)
				
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
	end
	
	if IO.const_defined?(:Buffer)
		with '#io_read' do
			let(:message) {"Hello World"}
			let(:events) {Array.new}
			let(:sockets) {UNIXSocket.pair}
			let(:local) {sockets.first}
			let(:remote) {sockets.last}
			
			let(:buffer) {IO::Buffer.new(1024, IO::Buffer::MAPPED)}
			
			it "can read a single message" do
				fiber = Fiber.new do
					events << :io_read
					offset = selector.io_read(Fiber.current, local, buffer, message.bytesize)
					expect(buffer.to_str(0, offset)).to be == message
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
				fiber = Fiber.new do
					events << :io_read
					offset = selector.io_read(Fiber.current, local, buffer, message.bytesize)
					expect(buffer.to_str(0, offset)).to be == message
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
		end
		
		with '#io_write', if: IO.const_defined?(:Buffer) do
			let(:message) {"Hello World"}
			let(:events) {Array.new}
			let(:sockets) {UNIXSocket.pair}
			let(:local) {sockets.first}
			let(:remote) {sockets.last}
			
			it "can write a single message" do
				fiber = Fiber.new do
					events << :io_write
					buffer = IO::Buffer.for(message)
					result = selector.io_write(Fiber.current, local, buffer, buffer.size)
					expect(result).to be == message.bytesize
					local.close
				end
				
				fiber.transfer
				
				selector.select(1)
				
				events << :read
				expect(remote.read).to be == message
				
				expect(events).to be == [
					:io_write, :read
				]
			end
		end
	end
	
	with '#process_wait' do
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
			
			selector.select(1)
			
			expect(events).to be == [:process_finished]
			expect(result.success?).to be == true
		end
		
		it "can wait for a process to terminate" do
			result = nil
			events = []
			
			fiber = Fiber.new do
				pid = Process.spawn("sleep 0.01")
				result = selector.process_wait(Fiber.current, pid, 0)
				expect(result).to be(:success?)
				events << :process_finished
			end
			
			fiber.transfer
			
			selector.select(2)
			
			expect(events).to be == [:process_finished]
			expect(result).to be(:success?)
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		with '.new' do
			def count = 8
			def loop = Fiber.current
			
			it "can create multiple selectors" do
				selectors = count.times.map do |i|
					subject.new(loop)
				end
				
				expect(selectors.size).to be == count
				
				selectors.each(&:close)
			end
		end

		with 'an instance' do
			def before
				@loop = Fiber.current
				@selector = subject.new(@loop)
			end
			
			def after
				@selector&.close
			end
			
			attr :loop
			attr :selector
			
			it_behaves_like Selector
		end
	end
end

describe IO::Event::Debug::Selector do
	def before
		@loop = Fiber.current
		@selector = subject.new(IO::Event::Selector.new(loop))
	end
	
	def after
		@selector&.close
	end
	
	attr :loop
	attr :selector
	
	it_behaves_like Selector
	
	with '#io_wait' do
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		it "cannot have two fibers reading from the same io" do
			fiber1 = Fiber.new do
				events << :wait_readable1
				selector.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable1
			rescue
				events << :error1
			end
			
			fiber2 = Fiber.new do
				events << :wait_readable2
				selector.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable2
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
				:error2,
				:select, :readable1
			]
		end
	end
end
