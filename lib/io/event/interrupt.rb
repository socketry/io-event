# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

module IO::Event
	# A thread safe synchronisation primative.
	class Interrupt
		def self.attach(selector)
			self.new(selector)
		end
		
		def initialize(selector)
			@selector = selector
			@input, @output = ::IO.pipe
			
			@output.sync = true
			
			@fiber = Fiber.new do
				while true
					if @selector.io_wait(@fiber, @input, IO::READABLE)
						@input.read_nonblock(1)
					end
				end
			rescue IOError
				# This is expected on shutdown.
			end
			
			@fiber.transfer
		end
		
		# Send a single byte interrupt.
		def signal
			# `signal` may be called while CRuby is waking a fiber blocked in `Thread#join`:
			#
			#   rb_fiber_scheduler_unblock -> TestScheduler#unblock -> Select#wakeup -> Interrupt#signal
			#
			# This path must not enter blocking IO. `IO#write` is a blocking operation and may
			# release the GVL or interact with scheduler/blocking-operation machinery. A wakeup
			# byte is best-effort, so use `write_nonblock` and ignore closed/full pipe errors.
			@output.write_nonblock(".") rescue nil
		end
		
		def close
			@input.close
			@output.close
		end
	end
	
	private_constant :Interrupt
end
