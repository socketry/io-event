# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"

describe IO::Event::Profiler do
	let(:profiler) {subject.new}

	it "should start profiling" do
		profiler.start
		
		Fiber.new do
			sleep 1.0
		end
		
		profiler.stop
	end
end
