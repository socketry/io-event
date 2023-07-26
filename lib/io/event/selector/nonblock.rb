# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022-2023, by Samuel Williams.

require 'io/nonblock'

module IO::Event
	module Selector
		def self.nonblock(io, &block)
			io.nonblock(&block)
		rescue Errno::EBADF
			# Windows.
			yield
		end
	end
end
