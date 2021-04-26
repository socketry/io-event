# Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

require "event/version"

module Event
	module Debug
		class Selector
			def initialize(selector)
				@selector = selector
				
				@readable = {}
				@writable = {}
				@priority = {}
			end
			
			def io_wait(fiber, io, events)
				register_readable(fiber, io, events)
			end
			
			def select(duration = nil)
				@selector.select(duration)
			end
			
			private
			
			def register_readable(fiber, io, events)
				if (events & IO::READABLE) > 0
					if @readable.key?(io)
						raise "Cannot wait for #{io} to become readable from multiple fibers."
					end
					
					begin
						@readable[io] = fiber
						
						register_writable(fiber, io, events)
					ensure
						@readable.delete(io)
					end
				else
					register_writable(fiber, io, events)
				end
			end
			
			def register_writable(fiber, io, events)
				if (events & IO::WRITABLE) > 0
					if @writable.key?(io)
						raise "Cannot wait for #{io} to become writable from multiple fibers."
					end
					
					begin
						@writable[io] = fiber
						
						register_priority(fiber, io, events)
					ensure
						@writable.delete(io)
					end
				else
					register_priority(fiber, io, events)
				end
			end
			
			def register_priority(fiber, io, events)
				if (events & IO::PRIORITY) > 0
					if @priority.key?(io)
						raise "Cannot wait for #{io} to become priority from multiple fibers."
					end
					
					begin
						@priority[io] = fiber
						
						@selector.io_wait(fiber, io, events)
					ensure
						@priority.delete(io)
					end
				else
					@selector.io_wait(fiber, io, events)
				end
			end
		end
	end
end
