# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022-2024, by Samuel Williams.

require "io/nonblock"

module IO::Event
	module Selector
		# Execute the given block in non-blocking mode.
		#
		# @parameter io [IO] The IO object to operate on.
		# @yields {...} The block to execute.
		def self.nonblock(io, &block)
			previous = io.nonblock?
			io.nonblock = true
		rescue Errno::EBADF, NotImplementedError
			# Windows.
			yield
		else
			begin
				yield
			ensure
				io.nonblock = previous
			end
		end
	end
end
