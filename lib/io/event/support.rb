# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022, by Samuel Williams.

require 'fiber'

unless Fiber.respond_to?(:blocking)
	class Fiber
		def self.blocking(&block)
			fiber = Fiber.new(blocking: true, &block)
			return fiber.resume(fiber)
		end
	end
end

class IO
	module Event
		module Support
			def self.buffer?
				IO.const_defined?(:Buffer) && IO::Buffer.instance_method(:read).arity == -1
			end
		end
	end
end