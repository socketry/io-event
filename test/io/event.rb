# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"

describe IO::Event::VERSION do
	it "has a version number" do
		expect(subject).to be =~ /\d+\.\d+\.\d+/
	end
end
