# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2022, by Samuel Williams.

require_relative '../support'

module IO::Event
	module Debug
		# Enforces the selector interface and delegates operations to a wrapped selector instance.
		class Selector
			def initialize(selector)
				@selector = selector
				
				@readable = {}
				@writable = {}
				@priority = {}
				
				unless Fiber.current == selector.loop
					Kernel::raise "Selector must be initialized on event loop fiber!"
				end
			end
			
			def wakeup
				@selector.wakeup
			end
			
			def close
				if @selector.nil?
					Kernel::raise "Selector already closed!"
				end
				
				@selector.close
				@selector = nil
			end
			
			# Transfer from the calling fiber to the event loop.
			def transfer
				@selector.transfer
			end
			
			def resume(*arguments)
				@selector.resume(*arguments)
			end
			
			def yield
				@selector.yield
			end
			
			def push(fiber)
				@selector.push(fiber)
			end
			
			def raise(fiber, *arguments)
				@selector.raise(fiber, *arguments)
			end
			
			def ready?
				@selector.ready?
			end
			
			def process_wait(*arguments)
				@selector.process_wait(*arguments)
			end
			
			def io_wait(fiber, io, events)
				@selector.io_wait(fiber, io, events)
			end
			
			def io_read(...)
				@selector.io_read(...)
			end
			
			def io_write(...)
				@selector.io_write(...)
			end
			
			def respond_to?(name, include_private = false)
				@selector.respond_to?(name, include_private)
			end
			
			def select(duration = nil)
				unless Fiber.current == @selector.loop
					Kernel::raise "Selector must be run on event loop fiber!"
				end
				
				@selector.select(duration)
			end
		end
	end
end
