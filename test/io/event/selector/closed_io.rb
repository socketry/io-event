# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

require_relative "../../../../fixtures/io/event/test_scheduler"

ClosedIO = Sus::Shared("closed io while selecting") do
	with "a pipe" do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		after do
			input.close unless input.closed?
			output.close unless output.closed?
		end
		
		it "does not raise when IO is closed from the same fiber before selecting" do
			thread = Thread.new do
				Thread.current.report_on_exception = false
				
				scheduler = IO::Event::TestScheduler.new(selector: subject.new(Fiber.current))
				Fiber.set_scheduler(scheduler)
				
				wait_fiber = Fiber.new do
					input.wait_readable
				rescue IOError
					# acceptable: the IO was closed while waiting
				end
				
				wait_fiber.transfer
				
				# Close the IO before calling select (deterministic, no race):
				input.close
				
				Thread.handle_interrupt(::SignalException => :never) do
					scheduler.selector.select(0)
				end
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end
			
			thread.join
		end
		
		it "does not raise when IO is closed from another thread while selecting" do
			thread = Thread.new do
				Thread.current.report_on_exception = false
				
				scheduler = IO::Event::TestScheduler.new(selector: subject.new(Fiber.current))
				Fiber.set_scheduler(scheduler)
				
				wait_fiber = Fiber.new do
					input.wait_readable
				rescue IOError
					# acceptable: the IO was closed while waiting
				end
				
				wait_fiber.transfer
				
				# Close the IO from another thread while the selector is blocking:
				closer = Thread.new do
					sleep(0.01)
					input.close
				end
				
				Thread.handle_interrupt(::SignalException => :never) do
					scheduler.selector.select(1.0)
				end
			ensure
				closer&.join
				Fiber.set_scheduler(nil)
				scheduler&.close
			end
			
			error = nil
			begin
				thread.join
			rescue => error
			end
			expect(error).to be_nil
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		it_behaves_like ClosedIO
	end
end
