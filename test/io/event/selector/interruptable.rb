# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2023-2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

Interruptable = Sus::Shared("interruptable") do
	it "ignores stale errno with pending interrupts" do
		selector = subject.new(Fiber.current)
		woken = false
		
		watchdog = Thread.new do
			sleep(0.1)
			woken = true
			selector.wakeup
		end
		
		result = nil
		
		begin
			begin
				Thread.handle_interrupt(::SignalException => :never) do
					# Keep an interrupt pending so Ruby may skip the native selector callback.
					Thread.current.raise(::Interrupt)
					
					# Leave an unrelated ENOENT in thread-local errno. A skipped callback must
					# not mistake this stale value for an error from epoll_wait/kevent.
					begin
						File.stat(__FILE__ + ".missing")
					rescue Errno::ENOENT
					end
					
					result = selector.select(nil)
				end
			rescue ::Interrupt
			end
		ensure
			watchdog.kill
			watchdog.join
		end
		
		expect(result).to be == 0
		expect(woken).to be == false
	end
	
	it "can interrupt sleeping selector" do
		result = nil
		
		thread = Thread.new do
			Thread.current.report_on_exception = false
			selector = subject.new(Fiber.current)
			
			Thread.handle_interrupt(::SignalException => :never) do
				result = selector.select(nil)
			end
		end
		
		# Wait for thread to enter the selector:
		sleep(0.001) until thread.status == "sleep"
		
		thread.raise(::Interrupt)
		
		expect{thread.join}.to raise_exception(::Interrupt)
		expect(result).to be == 0
	end
	
	with "pipe" do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		it "can interrupt waiting selector" do
			thread = Thread.new do
				Thread.current.report_on_exception = false
				selector = subject.new(Fiber.current)
				
				Fiber.new do
					selector.io_wait(Fiber.current, input, IO::READABLE)
				end
				
				Thread.handle_interrupt(::SignalException => :never) do
					selector.select(nil)
				end
			end
			
			# Wait for thread to enter the selector:
			sleep(0.001) until thread.status == "sleep"
			
			thread.raise(::Interrupt)
			
			expect{thread.join}.to raise_exception(::Interrupt)
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		it_behaves_like Interruptable
	end
end
