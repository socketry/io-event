# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require_relative "native"

module IO::Event
	unless self.const_defined?(:Profiler)
		module Profiler
		end
	end
end
