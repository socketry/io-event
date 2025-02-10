# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "native"

module IO::Event
	unless self.const_defined?(:Profiler)
		module Profiler
			# The default profiler, if the platform supports it.
			# Use `IO_EVENT_PROFILER=true` to enable it.
			def self.default
				nil
			end
		end
	end
end
