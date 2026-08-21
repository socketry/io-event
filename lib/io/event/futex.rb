# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

class IO
	module Event
		# Represents a Linux futex backed by an aligned 32-bit word in an {IO::Buffer}.
		#
		# This class is only defined when the native io_uring selector supports
		# `IORING_OP_FUTEX_WAIT`.
		class Futex
			# Wait while the futex contains the expected value.
			#
			# @asynchronous
			# @parameter expected [Integer] The value that must still be present before waiting.
			# @returns [Boolean] `true` when woken, or `false` when the value had already changed.
			# @raises [NotImplementedError] If the current fiber scheduler does not support futex waits.
			def wait(expected = value)
				scheduler = Fiber.scheduler
				
				unless scheduler&.respond_to?(:futex_wait)
					raise NotImplementedError, "The current fiber scheduler does not support futex waits!"
				end
				
				scheduler.futex_wait(self, expected)
			end
		end
	end
end
