# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2022, by Samuel Williams.

require_relative '../interrupt'
require_relative '../support'

module IO::Event
	module Selector
		class Select
			def initialize(loop)
				@loop = loop
				
				@waiting = Hash.new.compare_by_identity
				
				@blocked = false
				
				@ready = Queue.new
				@interrupt = Interrupt.attach(self)
			end
			
			attr :loop
			
			# If the event loop is currently sleeping, wake it up.
			def wakeup
				if @blocked
					@interrupt.signal
					
					return true
				end
				
				return false
			end
			
			def close
				@interrupt.close
				
				@loop = nil
				@waiting = nil
			end
			
			Optional = Struct.new(:fiber) do
				def transfer(*arguments)
					fiber&.transfer(*arguments)
				end
				
				def alive?
					fiber&.alive?
				end
				
				def nullify
					self.fiber = nil
				end
			end
			
			# Transfer from the current fiber to the event loop.
			def transfer
				@loop.transfer
			end
			
			# Transfer from the current fiber to the specified fiber. Put the current fiber into the ready list.
			def resume(fiber, *arguments)
				optional = Optional.new(Fiber.current)
				@ready.push(optional)
				
				fiber.transfer(*arguments)
			ensure
				optional.nullify
			end
			
			# Yield from the current fiber back to the event loop. Put the current fiber into the ready list.
			def yield
				optional = Optional.new(Fiber.current)
				@ready.push(optional)
				
				@loop.transfer
			ensure
				optional.nullify
			end
			
			# Append the given fiber into the ready list.
			def push(fiber)
				@ready.push(fiber)
			end
			
			# Transfer to the given fiber and raise an exception. Put the current fiber into the ready list.
			def raise(fiber, *arguments)
				optional = Optional.new(Fiber.current)
				@ready.push(optional)
				
				fiber.raise(*arguments)
			ensure
				optional.nullify
			end
			
			def ready?
				!@ready.empty?
			end
			
			Waiter = Struct.new(:fiber, :events, :tail) do
				def alive?
					self.fiber&.alive?
				end
				
				def transfer(events)
					if fiber = self.fiber
						self.fiber = nil
						
						fiber.transfer(events & self.events) if fiber.alive?
					end
				
					self.tail&.transfer(events)
				end
				
				def invalidate
					self.fiber = nil
				end
				
				def each(&block)
					if fiber = self.fiber
						yield fiber, self.events
					end
					
					self.tail&.each(&block)
				end
			end
			
			def io_wait(fiber, io, events)
				waiter = @waiting[io] = Waiter.new(fiber, events, @waiting[io])
				
				@loop.transfer
			ensure
				waiter&.invalidate
			end
			
			def io_select(readable, writable, priority, timeout)
				Thread.new do
					IO.select(readable, writable, priority, timeout)
				end.value
			end
			
			EAGAIN = -Errno::EAGAIN::Errno
			EWOULDBLOCK = -Errno::EWOULDBLOCK::Errno
			
			def again?(errno)
				errno == EAGAIN or errno == EWOULDBLOCK
			end
			
			if Support.fiber_scheduler_v2?
				def io_read(fiber, io, buffer, length, offset = 0)
					total = 0
					
					Selector.nonblock(io) do
						while true
							maximum_size = buffer.size - offset
							result = Fiber.blocking{buffer.read(io, maximum_size, offset)}
							
							if again?(result)
								if length > 0
									self.io_wait(fiber, io, IO::READABLE)
								else
									return result
								end
							elsif result < 0
								return result
							else
								total += result
								offset += result
								break if total >= length
							end
						end
					end
					
					return total
				end
				
				def io_write(fiber, io, buffer, length, offset = 0)
					total = 0
					
					Selector.nonblock(io) do
						while true
							maximum_size = buffer.size - offset
							result = Fiber.blocking{buffer.write(io, maximum_size, offset)}
							
							if again?(result)
								if length > 0
									self.io_wait(fiber, io, IO::READABLE)
								else
									return result
								end
							elsif result < 0
								return result
							else
								total += result
								offset += result
								break if total >= length
							end
						end
					end
					
					return total
				end
			elsif Support.fiber_scheduler_v1?
				def io_read(fiber, _io, buffer, length, offset = 0)
					io = IO.for_fd(_io.fileno, autoclose: false)
					total = 0
					
					while true
						maximum_size = buffer.size - offset
						
						case result = blocking{io.read_nonblock(maximum_size, exception: false)}
						when :wait_readable
							if length > 0
								self.io_wait(fiber, io, IO::READABLE)
							else
								return EWOULDBLOCK
							end
						when :wait_writable
							if length > 0
								self.io_wait(fiber, io, IO::WRITABLE)
							else
								return EWOULDBLOCK
							end
						when nil
							break
						else
							buffer.set_string(result, offset)
							
							size = result.bytesize
							total += size
							offset += size
							break if size >= length
							length -= size
						end
					end
					
					return total
				end
				
				def io_write(fiber, _io, buffer, length, offset = 0)
					io = IO.for_fd(_io.fileno, autoclose: false)
					total = 0
					
					while true
						maximum_size = buffer.size - offset
						
						chunk = buffer.get_string(offset, maximum_size)
						case result = blocking{io.write_nonblock(chunk, exception: false)}
						when :wait_readable
							if length > 0
								self.io_wait(fiber, io, IO::READABLE)
							else
								return EWOULDBLOCK
							end
						when :wait_writable
							if length > 0
								self.io_wait(fiber, io, IO::WRITABLE)
							else
								return EWOULDBLOCK
							end
						else
							total += result
							offset += result
							break if result >= length
							length -= result
						end
					end
					
					return total
				end
				
				def blocking(&block)
					fiber = Fiber.new(blocking: true, &block)
					return fiber.resume(fiber)
				end
			end
			
			def process_wait(fiber, pid, flags)
				Thread.new do
					Process::Status.wait(pid, flags)
				end.value
			end
			
			private def pop_ready
				unless @ready.empty?
					count = @ready.size
					
					count.times do
						fiber = @ready.pop
						fiber.transfer if fiber.alive?
					end
					
					return true
				end
			end
			
			def select(duration = nil)
				if pop_ready
					# If we have popped items from the ready list, they may influence the duration calculation, so we don't delay the event loop:
					duration = 0
				end
				
				readable = Array.new
				writable = Array.new
				priority = Array.new
				
				@waiting.each do |io, waiter|
					waiter.each do |fiber, events|
						if (events & IO::READABLE) > 0
							readable << io
						end
						
						if (events & IO::WRITABLE) > 0
							writable << io
						end
						
						if (events & IO::PRIORITY) > 0
							priority << io
						end
					end
				end
				
				@blocked = true
				duration = 0 unless @ready.empty?
				readable, writable, priority = ::IO.select(readable, writable, priority, duration)
				@blocked = false
				
				ready = Hash.new(0)
				
				readable&.each do |io|
					ready[io] |= IO::READABLE
				end
				
				writable&.each do |io|
					ready[io] |= IO::WRITABLE
				end
				
				priority&.each do |io|
					ready[io] |= IO::PRIORITY
				end
				
				ready.each do |io, events|
					@waiting.delete(io).transfer(events)
				end
				
				return ready.size
			end
		end
	end
end
