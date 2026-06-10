# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024-2025, by Samuel Williams.

require_relative "priority_heap"

class IO
	module Event
		# An efficient sorted set of timers.
		class Timers
			COMPACT_MINIMUM_COUNT = 128
			
			# A handle to a scheduled timer.
			class Handle
				# Initialize the handle with the given time and block.
				#
				# @parameter time [Float] The time at which the block should be called.
				# @parameter block [Proc] The block to call.
				def initialize(time, block)
					@timers = nil
					@time = time
					@block = block
				end
				
				# @attribute [Float] The time at which the block should be called.
				attr :time
				
				# @attribute [Proc | Nil] The block to call when the timer fires.
				attr :block
				
				# Mark the timer as inserted into the heap.
				def schedule!(timers)
					@timers = timers
				end
				
				# Mark the timer as removed from the heap.
				def removed!
					@timers = nil
				end
				
				# Compare the handle with another handle.
				#
				# @parameter other [Handle] The other handle to compare with.
				# @returns [Boolean] Whether the handle is less than the other handle.
				def < other
					@time < other.time
				end
				
				# Compare the handle with another handle.
				#
				# @parameter other [Handle] The other handle to compare with.
				# @returns [Boolean] Whether the handle is greater than the other handle.
				def > other
					@time > other.time
				end
				
				# Invoke the block.
				def call(...)
					@block.call(...)
				end
				
				# Cancel the timer.
				def cancel!
					return if @block.nil?
					
					@block = nil
					
					if timers = @timers
						@timers = nil
						timers.cancelled!(self)
					end
				end
				
				# @returns [Boolean] Whether the timer has been cancelled.
				def cancelled?
					@block.nil?
				end
			end
			
			# Initialize the timers.
			def initialize
				@heap = PriorityHeap.new
				@scheduled = []
				@cancelled = 0
			end
			
			# @returns [Integer] The number of timers in the heap.
			def size
				flush!
				compact!
				
				return @heap.size
			end
			
			# Schedule a block to be called at a specific time in the future.
			#
			# @parameter time [Float] The time at which the block should be called, relative to {#now}.
			# @parameter block [Proc] The block to call.
			def schedule(time, block)
				handle = Handle.new(time, block)
				
				@scheduled << handle
				
				return handle
			end
			
			# Schedule a block to be called after a specific time offset, relative to the current time as returned by {#now}.
			#
			# @parameter offset [#to_f] The time offset from the current time at which the block should be called.
			# @yields {|now| ...} When the timer fires.
			def after(offset, &block)
				schedule(self.now + offset.to_f, block)
			end
			
			# Compute the time interval until the next timer fires.
			#
			# @parameter now [Float] The current time.
			# @returns [Float | Nil] The time interval until the next timer fires, if any.
			def wait_interval(now = self.now)
				flush!
				compact!
				
				while handle = @heap.peek
					if handle.cancelled?
						@heap.pop
						handle.removed!
						@cancelled -= 1 if @cancelled > 0
					else
						return handle.time - now
					end
				end
			end
			
			# @returns [Float] The current time.
			def now
				::Process.clock_gettime(::Process::CLOCK_MONOTONIC)
			end
			
			# Fire all timers that are ready to fire.
			#
			# @parameter now [Float] The current time.
			def fire(now = self.now)
				# Flush scheduled timers into the heap:
				flush!
				compact!
				
				# Get the earliest timer:
				while handle = @heap.peek
					if handle.cancelled?
						@heap.pop
						handle.removed!
						@cancelled -= 1 if @cancelled > 0
					elsif handle.time <= now
						# Remove the earliest timer from the heap:
						@heap.pop
						handle.removed!
						
						# Call the block:
						handle.call(now)
					else
						break
					end
				end
			end
			
			# Flush all scheduled timers into the heap.
			#
			# This is a small optimization which assumes that most timers (timeouts) will be cancelled.
			protected def flush!
				while handle = @scheduled.pop
					unless handle.cancelled?
						handle.schedule!(self)
						@heap.push(handle)
					end
				end
			end
			
			# Periodically rebuild the heap when cancelled timers become a large
			# enough fraction of the heap to risk unbounded retention or pruning spikes.
			protected def compact!
				if @cancelled == @heap.size
					@heap.clear!
					@cancelled = 0
				elsif @cancelled >= COMPACT_MINIMUM_COUNT && @cancelled * 2 > @heap.size
					@heap.delete_if do |handle|
						if handle.cancelled?
							handle.removed!
							true
						end
					end
					
					@cancelled = 0
				end
			end
			
			# Track cancelled timers that are still retained in the heap.
			def cancelled!(handle)
				@cancelled += 1
			end
		end
	end
end
