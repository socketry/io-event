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
			
			if IO.const_defined?(:Buffer)
				def io_read(fiber, io, buffer, length)
					@selector.io_read(fiber, io, buffer, length)
				end
				
				def io_write(fiber, io, buffer, length)
					@selector.io_write(fiber, io, buffer, length)
				end
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
