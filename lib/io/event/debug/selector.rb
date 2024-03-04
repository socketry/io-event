# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require_relative '../support'

module IO::Event
	module Debug
		# Enforces the selector interface and delegates operations to a wrapped selector instance.
		class Selector
			def self.wrap(selector, env = ENV)
				log = nil
				
				if log_path = env['IO_EVENT_DEBUG_SELECTOR_LOG']
					log = File.open(log_path, 'w')
				end
				
				return self.new(selector, log: log)
			end
			
			def initialize(selector, log: nil)
				@selector = selector
				
				@readable = {}
				@writable = {}
				@priority = {}
				
				unless Fiber.current == selector.loop
					Kernel::raise "Selector must be initialized on event loop fiber!"
				end
				
				@log = log
			end
			
			def idle_duration
				@selector.idle_duration
			end
			
			def now
				Process.clock_gettime(Process::CLOCK_MONOTONIC)
			end
			
			def log(message)
				return unless @log
				
				Fiber.blocking do
					@log.puts("T+%10.1f; %s" % [now, message])
				end
			end
			
			def wakeup
				@selector.wakeup
			end
			
			def close
				log("Closing selector")
				
				if @selector.nil?
					Kernel::raise "Selector already closed!"
				end
				
				@selector.close
				@selector = nil
			end
			
			# Transfer from the calling fiber to the event loop.
			def transfer
				log("Transfering to event loop")
				@selector.transfer
			end
			
			def resume(*arguments)
				log("Resuming fiber with #{arguments.inspect}")
				@selector.resume(*arguments)
			end
			
			def yield
				log("Yielding to event loop")
				@selector.yield
			end
			
			def push(fiber)
				log("Pushing fiber #{fiber.inspect} to ready list")
				@selector.push(fiber)
			end
			
			def raise(fiber, *arguments)
				log("Raising exception on fiber #{fiber.inspect} with #{arguments.inspect}")
				@selector.raise(fiber, *arguments)
			end
			
			def ready?
				@selector.ready?
			end
			
			def process_wait(*arguments)
				log("Waiting for process with #{arguments.inspect}")
				@selector.process_wait(*arguments)
			end
			
			def io_wait(fiber, io, events)
				log("Waiting for IO #{io.inspect} for events #{events.inspect}")
				@selector.io_wait(fiber, io, events)
			end
			
			def io_read(fiber, io, buffer, length, offset = 0)
				log("Reading from IO #{io.inspect} with buffer #{buffer}; length #{length} offset #{offset}")
				@selector.io_read(fiber, io, buffer, length, offset)
			end
			
			def io_write(fiber, io, buffer, length, offset = 0)
				log("Writing to IO #{io.inspect} with buffer #{buffer}; length #{length} offset #{offset}")
				@selector.io_write(fiber, io, buffer, length, offset)
			end
			
			def respond_to?(name, include_private = false)
				@selector.respond_to?(name, include_private)
			end
			
			def select(duration = nil)
				log("Selecting for #{duration.inspect}")
				unless Fiber.current == @selector.loop
					Kernel::raise "Selector must be run on event loop fiber!"
				end
				
				@selector.select(duration)
			end
		end
	end
end
