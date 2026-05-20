# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"

describe IO::Event::Selector do
	with "URing" do
		it "does not expose unmatched poll completions as integer zero from io_wait" do
			skip "URing is not available" unless subject.const_defined?(:URing)
			
			uring = subject.const_get(:URing)
			
			# io_uring poll completions can include flags we did not request. The
			# current implementation filters the raw poll result before translating
			# it to Ruby IO events, which can produce Integer(0). Ruby's socket
			# connect path treats any non-false, non-negative result as success.
			result = uring.__test_io_wait_unmatched_poll_result(IO::READABLE | IO::WRITABLE)
			
			expect(result).not.to be == 0
		end
	end
end
