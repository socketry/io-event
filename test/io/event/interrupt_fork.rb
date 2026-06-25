# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/interrupt"
require "io/event/test_scheduler"

describe IO::Event.const_get(:Interrupt) do
	with "test scheduler fork diagnostics" do
		it "can be used to wake up a fiber blocked in `Thread#join` after fork" do
			skip "Process.fork is not available on JRuby" if RUBY_ENGINE == "jruby"
			
			iterations = Integer(ENV.fetch("IO_EVENT_FORK_ITERATIONS", "100"))
			
			iterations.times do |iteration|
				r, w = IO.pipe
				
				Thread.new do
					selector = IO::Event::Selector::Select.new(Fiber.current)
					scheduler = IO::Event::TestScheduler.new(selector: selector)
					Fiber.set_scheduler(scheduler)
					
					Fiber.schedule do
						selector.dump_state($stderr, label: "interrupt_fork iteration #{iteration + 1} before fork") if ENV["IO_EVENT_DIAGNOSTICS"]
						
						pid = Process.fork do
							w.write("hello")
						end
						
						w.close
						expect(r.read).to be == "hello"
					ensure
						Fiber.blocking do
							$stderr.puts "Waiting for child process #{pid} to exit... (#{$!})"
						end
						
						Process.waitpid(pid) if pid
					end
				end.join
			ensure
				r&.close unless r&.closed?
				w&.close unless w&.closed?
			end
		end
	end
end
