# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024, by Samuel Williams.

require_relative 'priority_heap'

class IO
	module Event
		class Timers
			class Handle
				def initialize(time, block)
					@time = time
					@block = block
				end
				
				def < other
					@time < other.time
				end
				
				def > other
					@time > other.time
				end
				
				attr :time
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
			
			# Schedule a block to be called at a specific time in the future.
			# @parameter time [Float] The time at which the block should be called, relative to {#now}.
			def schedule(time, block)
				handle = Handle.new(time, block)
				
				@scheduled << handle
				
				return handle
			end
			
			# Schedule a block to be called after a specific time offset, relative to the current time as returned by {#now}.
			# @parameter offset [#to_f] The time offset from the current time at which the block should be called.
			def after(offset, &block)
				schedule(self.now + offset.to_f, block)
			end
			
			def wait_interval(now = self.now)
				flush!
				
				while handle = @heap.peek
					if handle.cancelled?
						@heap.pop
					else
						return handle.time - now
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
					elsif handle.time <= now
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
