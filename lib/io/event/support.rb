# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022, by Samuel Williams.

class IO
	module Event
		module Support
			def buffer?
				IO.const_defined?(:Buffer)
			end
			
			def self.fiber_scheduler_v1?
				IO.const_defined?(:Buffer) && !Fiber.respond_to?(:blocking)
			end
			
			def self.fiber_scheduler_v2?
				IO.const_defined?(:Buffer) && Fiber.respond_to?(:blocking)
			end
		end
	end
end
