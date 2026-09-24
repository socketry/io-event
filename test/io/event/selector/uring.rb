# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"

return unless defined?(IO::Event::Selector::URing)

describe IO::Event::Selector::URing do
	it "falls back when newer setup flags are unavailable" do
		result = subject.test_setup_flag_fallback
		skip "No optional setup flags are available" unless result
		
		status, attempts = result
		
		expect(status).to be == 0
		expect(attempts).to be > 1
	end
end
