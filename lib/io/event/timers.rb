# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024, by Samuel Williams.

require_relative 'priority_heap'

class IO
	module Event
		class Timers
			class Handle
				def initialize(offset, block)
					@offset = offset
					@block = block
				end
				
				def < other
					@offset < other.offset
				end
				
				def > other
					@offset > other.offset
				end
				
				attr :offset
				attr :block
				
				def call(...)
					@block.call(...)
				end
				
				def cancel!
					@block = nil
				end
				
				def cancelled?
					@block.nil?
				end
			end
			
			def initialize
				@heap = PriorityHeap.new
				@scheduled = []
			end
			
			def size
				flush!
				
				return @heap.size
			end
			
			def schedule(offset, block)
				handle = Handle.new(offset, block)
				@scheduled << handle
				
				return handle
			end
			
			def after(timeout, &block)
				schedule(now + timeout, block)
			end
			
			def wait_interval(now = self.now)
				flush!
				
				while handle = @heap.peek
					if handle.cancelled?
						@heap.pop
					else
						return handle.offset - now
					end
				end
			end
			
			def now
				::Process.clock_gettime(::Process::CLOCK_MONOTONIC)
			end
			
			def fire(now = self.now)
				# Flush scheduled timers into the heap:
				flush!
				
				# Get the earliest timer:
				while handle = @heap.peek
					if handle.cancelled?
						@heap.pop
					elsif handle.offset <= now
						# Remove the earliest timer from the heap:
						@heap.pop
						
						# Call the block:
						handle.call(now)
					else
						break
					end
				end
			end
			
			protected def flush!
				while handle = @scheduled.pop
					@heap.push(handle) unless handle.cancelled?
				end
			end
		end
	end
end
