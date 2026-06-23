# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/interrupt"
require "io/event/selector/select"
require "io/event/test_scheduler"

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
	
	with "#signal" do
		it "does not use blocking IO when waking a thread joiner" do
			selector = IO::Event::Selector::Select.new(Fiber.current)
			scheduler = IO::Event::TestScheduler.new(selector: selector)
			
			interrupt = selector.instance_variable_get(:@interrupt)
			output = interrupt.instance_variable_get(:@output)
			error_class = Class.new(Exception)
			
			# CRuby's `Thread#join` with a fiber scheduler blocks the joining fiber
			# with `scheduler.block`. When the target thread exits, CRuby wakes the
			# joiner by calling `scheduler.unblock` from the exiting thread. In the
			# test scheduler, that becomes:
			#
			#   TestScheduler#unblock -> Select#wakeup -> Interrupt#signal
			#
			# If `Interrupt#signal` uses blocking `IO#write`, an exception or other
			# blocking-IO side effect can propagate through the target thread and show
			# up as a `Thread#join` failure. `write_nonblock` avoids that path.
			output.define_singleton_method(:write) do |*|
				raise error_class, "blocking write used"
			end
			
			Fiber.set_scheduler(scheduler)
			
			thread = nil
			joined = nil
			error = nil
			
			Fiber.schedule do
				thread = Thread.new do
					sleep 0.01
				end
				
				begin
					joined = thread.join(0.05)
				rescue Exception => exception
					error = exception
				end
			end
			
			scheduler.run
			
			expect(error).to be_nil
			expect(joined).to be == thread
		ensure
			Fiber.set_scheduler(nil)
		end
	end
end
