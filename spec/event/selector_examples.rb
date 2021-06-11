# Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
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

require 'event/selector'
require 'socket'

RSpec.shared_examples_for Event::Selector do
	let!(:loop) {Fiber.current}
	subject{described_class.new(loop)}
	
	after do
		subject.close
	end
	
	describe '.new' do
		it "can create multiple selectors" do
			8.times.map do |i|
				described_class.new(Fiber.current)
			end.each(&:close)
		end
	end
	
	describe '.select' do
		let(:quantum) {0.2}
		
		def now
			Process.clock_gettime(Process::CLOCK_MONOTONIC)
		end
		
		it "can select with 0s timeout" do
			start_time = now
			subject.select(0)
			
			expect(now - start_time).to be < quantum
		end
		
		it "can select with 1s timeout" do
			start_time = now
			subject.select(1)
			
			expect(now - start_time).to be_within(quantum).of(1.0)
		end
	end
	
	describe '#io_wait' do
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		it "can wait for an io to become readable" do
			fiber = Fiber.new do
				events << :wait_readable
				
				expect(
					subject.io_wait(Fiber.current, local, Event::READABLE)
				).to be == Event::READABLE
				
				events << :readable
			end
			
			events << :transfer
			fiber.transfer
			
			remote.puts "Hello World"
			
			events << :select
			
			subject.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable,
				:select, :readable
			]
		end
		
		it "can wait for an io to become writable" do
			fiber = Fiber.new do
				events << :wait_writable
				
				expect(
					subject.io_wait(Fiber.current, local, Event::WRITABLE)
				).to be == Event::WRITABLE
				
				events << :writable
			end
			
			events << :transfer
			fiber.transfer
			
			events << :select
			subject.select(1)
			
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
					subject.io_wait(Fiber.current, local, Event::READABLE)
				).to be == Event::READABLE
				
				readable = true
			end
			
			write_fiber = Fiber.new do
				events << :wait_writable
				
				expect(
					subject.io_wait(Fiber.current, local, Event::WRITABLE)
				).to be == Event::WRITABLE
				
				writable = true
			end
			
			events << :transfer
			read_fiber.transfer
			write_fiber.transfer
			
			remote.puts "Hello World"
			events << :select
			subject.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable, :wait_writable,
				:select
			]
			
			expect(readable).to be true
			expect(writable).to be true
		end
		
		it "can handle exception during wait" do
			fiber = Fiber.new do
				events << :wait_readable
				
				expect do
					while true
						subject.io_wait(Fiber.current, local, Event::READABLE)
						events << :readable
					end
				end.to raise_exception(RuntimeError, /Boom/)
				
				events << :error
			end
			
			events << :transfer
			fiber.transfer
			
			events << :select
			subject.select(0)
			fiber.raise(RuntimeError.new("Boom"))
			
			events << :puts
			remote.puts "Hello World"
			subject.select(0)
			
			expect(events).to be == [
				:transfer, :wait_readable,
				:select, :error, :puts
			]
		end
	end
	
	describe '#process_wait' do
		let!(:loop) {Fiber.current}
		subject{described_class.new(loop)}
		
		it "can wait for a process which has terminated already" do
			result = nil
			events = []
			
			fiber = Fiber.new do
				pid = Process.spawn("true")
				sleep(1)
				
				result = subject.process_wait(Fiber.current, pid, 0)
				pp result
				events << :process_finished
			end
			
			fiber.transfer
			
			subject.select(1)
			
			expect(events).to be == [:process_finished]
			expect(result.success?).to be == true
		end
		
		it "can wait for a process to terminate" do
			result = nil
			events = []
			
			fiber = Fiber.new do
				pid = Process.spawn("sleep 1")
				result = subject.process_wait(Fiber.current, pid, 0)
				pp result
				events << :process_finished
			end
			
			fiber.transfer
			
			subject.select(2)
			
			expect(events).to be == [:process_finished]
			expect(result.success?).to be == true
		end
	end
end
