# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/interrupt"
require "io/event/test_scheduler"
require "io/nonblock"

describe IO::Event.const_get(:Interrupt) do
	let(:loop) {Fiber.current}
	
	let(:selector) do
		loop = self.loop
		
		Object.new.tap do |selector|
			selector.define_singleton_method(:io_wait) do |fiber, io, events|
				loop.transfer
				
				return true
			end
		end
	end
	
	with "#close" do
		it "handles IOError in the interrupt fiber" do
			interrupt = subject.new(selector)
			fiber = interrupt.instance_variable_get(:@fiber)
			
			interrupt.close
			
			expect do
				fiber.transfer
			end.not.to raise_exception(IOError)
			
			expect(fiber).not.to be(:alive?)
		end
	end
	
	with "test scheduler" do
		it "can be used to wake up a fiber blocked in `Thread#join`" do
			100.times do
				r, w = IO.pipe
				
				Thread.new do
					selector = IO::Event::Selector::Select.new(Fiber.current)
					scheduler = IO::Event::TestScheduler.new(selector: selector)
					Fiber.set_scheduler(scheduler)
					
					Fiber.schedule do
						pid = Process.fork do
							# Child process:
							w.write("hello")
						end
						
						# Parent process:
						w.close
						expect(r.read).to be == "hello"
					ensure
						Process.waitpid(pid) if pid
					end
				end.join
			end
		end
	end
	
	with "#signal" do
		it "does not block when the interrupt pipe is full" do
			interrupt = subject.new(selector)
			output = interrupt.instance_variable_get(:@output)
			
			begin
				output.nonblock = true
				
				while true
					output.write_nonblock("." * 4096)
				end
			rescue IO::WaitWritable
				output.nonblock = false
			end
			
			error = nil
			completed = false
			
			thread = Thread.new do
				begin
					interrupt.signal
					completed = true
				rescue Exception => exception
					error = exception
				end
			end
			
			thread.join(0.1)
			
			# If `Interrupt#signal` uses blocking `IO#write`, this thread remains
			# blocked in the write until `Interrupt#close` closes the output pipe.
			# `write_nonblock` returns immediately instead.
			expect(error).to be_nil
			expect(completed).to be == true
		ensure
			interrupt&.close
			thread&.join
		end
	end
end
