# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"

return unless defined?(IO::Event::Selector::URing)

describe IO::Event::Selector::URing do
	it "releases completion state when SQE acquisition fails" do
		pid = Process.fork do
			selector = subject.new(Fiber.current)
			input, output = IO.pipe
			error = false
			
			selector.send(:test_fail_next_sqe, Errno::EIO::Errno)
			
			begin
				selector.io_wait(Fiber.current, input, IO::READABLE)
			rescue Errno::EIO
				error = true
			end
			
			pending = selector.send(:test_pending_completions)
			exit!(error && pending == 0 ? 0 : 1)
		end
		
		_, status = Process.wait2(pid)
		expect(status).to be(:success?)
	end
end
