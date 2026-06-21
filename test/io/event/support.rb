# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/support"

describe IO::Event::Support do
	it "can report buffer support" do
		expect(subject.buffer?).to be == IO.const_defined?(:Buffer)
	end
end
