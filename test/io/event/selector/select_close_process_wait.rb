# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector/select"

require "rbconfig"

require_relative "../../../../fixtures/io/event/test_scheduler"

class SelectCloseProcessWaitScheduler < IO::Event::TestScheduler
	def io_read(io, buffer, length, offset = 0)
		@selector.io_read(Fiber.current, io, buffer, length, offset)
	end
	
	def io_write(io, buffer, length, offset = 0)
		@selector.io_write(Fiber.current, io, buffer, length, offset)
	end
	
	def process_wait(pid, flags)
		@selector.process_wait(Fiber.current, pid, flags)
	end
end

describe IO::Event::Selector::Select do
	with "IO#close while waiting for a process" do
		it "does not interrupt process wait helper threads" do
			skip_unless_minimum_ruby_version("4.1")
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			iterations = Integer(ENV.fetch("IO_EVENT_SELECT_CLOSE_PROCESS_WAIT_ITERATIONS", "20000"))
			timeout = Integer(ENV.fetch("IO_EVENT_SELECT_CLOSE_PROCESS_WAIT_TIMEOUT", "120"))
			
			status, stdout, stderr = run_select_close_process_wait_repro(iterations, timeout)
			
			unless status.success?
				raise "child process failed with #{status.inspect}:\n#{stdout}\n#{stderr}"
			end
		end
	end
end

def run_select_close_process_wait_repro(iterations, timeout)
	script = <<~'RUBY'
		require "io/event"
		require "io/event/selector/select"
		
		require_relative "fixtures/io/event/test_scheduler"
		
		class SelectCloseProcessWaitScheduler < IO::Event::TestScheduler
			def io_read(io, buffer, length, offset = 0)
				@selector.io_read(Fiber.current, io, buffer, length, offset)
			end
			
			def io_write(io, buffer, length, offset = 0)
				@selector.io_write(Fiber.current, io, buffer, length, offset)
			end
			
			def process_wait(pid, flags)
				@selector.process_wait(Fiber.current, pid, flags)
			end
		end
		
		ITERATIONS = Integer(ENV.fetch("ITERATIONS"))
		
		Thread.report_on_exception = true
		Thread.abort_on_exception = true
		
		ITERATIONS.times do |iteration|
			input, output = IO.pipe
			
			thread = Thread.new do
				scheduler = SelectCloseProcessWaitScheduler.new(
					selector: IO::Event::Selector::Select.new(Fiber.current)
				)
				
				Fiber.set_scheduler(scheduler)
				
				reader = Fiber.new do
					input.read(5)
					raise "read unexpectedly completed"
				rescue IOError => error
					raise unless error.message.match?(/closed/)
				end
				
				waiter = Fiber.new do
					pid = Process.spawn("true")
					_, status = Process.wait2(pid)
					raise "child process failed" unless status.success?
				end
				
				closer = Fiber.new do
					sleep(0.001)
					input.close
				end
				
				reader.transfer
				waiter.transfer
				closer.transfer
				
				scheduler.run
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end
			
			begin
				thread.value
			ensure
				output.close unless output.closed?
				input.close unless input.closed?
			end
			
			$stdout.puts "completed #{iteration + 1}/#{ITERATIONS}" if ((iteration + 1) % 1000).zero?
		end
	RUBY
	
	stdout_reader, stdout_writer = IO.pipe
	stderr_reader, stderr_writer = IO.pipe
	
	pid = Process.spawn(
		{"ITERATIONS" => iterations.to_s},
		RbConfig.ruby, "-Ilib", "-Ifixtures", "-e", script,
		out: stdout_writer,
		err: stderr_writer
	)
	
	stdout_writer.close
	stderr_writer.close
	
	stdout_thread = Thread.new{stdout_reader.read}
	stderr_thread = Thread.new{stderr_reader.read}
	
	deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + timeout
	status = nil
	
	while Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
		_, status = Process.wait2(pid, Process::WNOHANG)
		break if status
		
		sleep 0.1
	end
	
	unless status
		Process.kill(:TERM, pid)
		
		10.times do
			_, status = Process.wait2(pid, Process::WNOHANG)
			break if status
			
			sleep 0.1
		end
		
		unless status
			Process.kill(:KILL, pid)
			_, status = Process.wait2(pid)
		end
	end
	
	stdout = stdout_thread.value
	stderr = stderr_thread.value
	
	return status, stdout, stderr
ensure
	stdout_reader&.close unless stdout_reader&.closed?
	stderr_reader&.close unless stderr_reader&.closed?
end
