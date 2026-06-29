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
			skip_unless_method_defined(:fork, Process.singleton_class)
			skip "Process.fork is not available on JRuby." if RUBY_ENGINE == "jruby"
			skip "Fiber.set_scheduler is not available." unless Fiber.respond_to?(:set_scheduler)
			
			10.times do
				r, w = IO.pipe
				
				Thread.new do
					selector = IO::Event::Selector::Select.new(Fiber.current)
					scheduler = IO::Event::TestScheduler.new(selector: selector)
					Fiber.set_scheduler(scheduler)
					
					Fiber.schedule do
						selector.dump_state($stderr, label: "interrupt fork before fork") if ENV["IO_EVENT_DIAGNOSTICS"]
						
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
end
