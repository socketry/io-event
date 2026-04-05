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
				
				# Close must happen in a separate fiber so that rb_thread_io_close_wait
				# can yield (via kernel_sleep) back to the loop fiber instead of deadlocking:
				close_fiber = Fiber.new do
					input.close
				end
				
				wait_fiber.transfer
				close_fiber.transfer
				
				scheduler.run
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
				
				scheduler.run
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
