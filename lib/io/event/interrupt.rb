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
			# This must not block or enter blocking operations or raise an exception.
			@output.write_nonblock(".", exception: false)
		end
		
		def close
			@input.close
			@output.close
		end
	end
	
	private_constant :Interrupt
end
