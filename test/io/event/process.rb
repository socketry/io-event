# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "io/event/test_scheduler"

# Integration tests for the scheduler-dependent behaviour of `process_wait`. These properties only hold when a fiber scheduler is installed, so they are exercised here through `TestScheduler` rather than at the bare-selector level.
ProcessWait = Sus::Shared("process wait") do
	it "does not block the reactor while waiting for any child process" do
		skip_if_ruby_platform(/mswin|mingw|cygwin/)
		
		order = []
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			Process.spawn("sleep 0.2")
			Process.wait(-1)
			order << :process
		end
		
		Fiber.schedule do
			sleep(0.01)
			order << :sleep
		end
		
		scheduler.run
		
		# If waiting for the process had blocked the reactor, the short sleep could not have completed first:
		expect(order).to be == [:sleep, :process]
	ensure
		Fiber.set_scheduler(nil)
	end
	
	it "ignores stale scheduler wake-ups while waiting in a worker thread" do
		# Pending backports of https://github.com/ruby/ruby/pull/13532:
		# - Ruby 3.3: https://github.com/ruby/ruby/pull/18502
		# - Ruby 3.4: https://github.com/ruby/ruby/pull/18503
		skip_unless_minimum_ruby_version("4")
		skip_if_ruby_platform(/mswin|mingw|cygwin/)
		
		input, output = IO.pipe
		pid = Process.spawn("cat", in: input, out: File::NULL)
		input.close
		input = nil
		status = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			# Simulate a deferred wake-up from a previous blocking operation:
			scheduler.unblock(nil, Fiber.current)
			
			status = IO::Event::Selector.process_wait(pid, 0)
		end
		
		Fiber.schedule do
			# Release the child after the stale wake-up has been delivered:
			sleep(0.01)
			output.close
			output = nil
		end
		
		scheduler.run
		
		expect(status).to be(:success?)
	ensure
		input&.close
		output&.close
		
		if pid
			Process.kill(:KILL, pid) rescue nil
			Process.wait(pid) rescue nil
		end
		
		Fiber.set_scheduler(nil)
	end
	
	it "can wait for all child processes" do
		skip_if_ruby_platform(/mswin|mingw|cygwin/)
		
		statuses = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			3.times{Process.spawn("true")}
			
			# `Process.waitall` loops `Process.wait(-1)` until `ECHILD`, relying on the final wait reporting "no more children" rather than raising. The scheduler hook must surface that as a result, not an exception:
			statuses = Process.waitall
		end
		
		scheduler.run
		
		expect(statuses.size).to be == 3
		statuses.each do |pid, status|
			expect(status).to be(:success?)
		end
	ensure
		Fiber.set_scheduler(nil)
	end
	
	it "raises when waiting for any child process with no children" do
		skip_if_ruby_platform(/mswin|mingw|cygwin/)
		
		error = nil
		
		Fiber.set_scheduler(scheduler)
		
		Fiber.schedule do
			Process.wait(-1)
		rescue SystemCallError => exception
			error = exception
		end
		
		scheduler.run
		
		# Consistent with non-scheduler `Process.wait(-1)`: no children is `Errno::ECHILD`.
		expect(error).to be_a(Errno::ECHILD)
	ensure
		Fiber.set_scheduler(nil)
	end
	
	it "can interrupt a process wait" do
		skip_if_ruby_platform(/mswin|mingw|cygwin/)
		
		pid = Process.spawn("sleep 10")
		error = nil
		
		Fiber.set_scheduler(scheduler)
		
		waiter = Fiber.schedule do
			Process.wait(-1)
		rescue => exception
			error = exception
		end
		
		Fiber.schedule do
			scheduler.fiber_interrupt(waiter, StandardError.new("Interrupted!"))
		end
		
		scheduler.run
		
		expect(error).to be_a(StandardError)
	ensure
		Process.kill(:KILL, pid) rescue nil
		Process.wait(pid) rescue nil
		Fiber.set_scheduler(nil)
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		let(:selector) {klass.new(Fiber.current)}
		let(:scheduler) {IO::Event::TestScheduler.new(selector: selector)}
		
		it_behaves_like ProcessWait
	end
end
