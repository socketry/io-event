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

module Event
	module Backend
		class Select
			def initialize(loop)
				@loop = loop
				
				@readable = {}
				@writable = {}
			end
			
			def io_wait(fiber, io, events)
				remove_readable = remove_writable = false
				
				if (events & READABLE) > 0 or (events & PRIORITY) > 0
					@readable[io] = fiber
					remove_readable = true
				end
				
				if (events & WRITABLE) > 0
					@writable[io] = fiber
					remove_writable = true
				end
				
				@loop.transfer
				
			ensure
				@readable.delete(io) if remove_readable
				@writable.delete(io) if remove_writable
			end

			def process_wait(fiber, pid, flags)
				Thread.new do
					val = Process::Status.wait(pid, flags)
					fiber.transfer(val)
				end
				@loop.transfer
			end
			
			def select(duration = nil)
				readable, writable, _ = ::IO.select(@readable.keys, @writable.keys, nil, duration)
				
				ready = Hash.new(0)
				
				readable&.each do |io|
					fiber = @readable.delete(io)
					ready[fiber] |= READABLE
				end
				
				writable&.each do |io|
					fiber = @writable.delete(io)
					ready[fiber] |= WRITABLE
				end
				
				ready.each do |fiber, events|
					fiber.transfer(events)
				end
			end
		end
	end
end
