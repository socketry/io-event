# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022-2023, by Samuel Williams.

unless Object.const_defined?(:UNIXSocket)
	class UNIXSocket
		def self.pair(&block)
			Socket.pair(:INET, :STREAM, 0, &block)
		end
	end
end
