# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"

return unless defined?(IO::Event::Selector::URing)

describe IO::Event::Selector::URing do
	let(:selector) {subject.new(Fiber.current)}

	after do
		selector&.close
	end

	it "returns control when submission encounters backpressure" do
		result, submissions = selector.send(:test_submission_backpressure)

		expect(result).to be == -Errno::EAGAIN::Errno
		expect(submissions).to be == 1
	end
end
