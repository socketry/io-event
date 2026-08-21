# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

class IO
	module Event
		# Represents a Linux futex backed by an aligned 32-bit word in an {IO::Buffer}.
		#
		# This class is only defined when the native io_uring selector supports
		# `IORING_OP_FUTEX_WAIT`.
		#
		# The buffer must not be explicitly freed or resized while a futex refers
		# to it. The futex retains the buffer so it cannot be garbage collected.
		class Futex
		end
	end
end
