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

require_relative 'selector'

module Event
	# A thread safe synchronisation primative.
	class Interrupt
		def self.attach(selector, &block)
			self.new(selector, block)
		end
		
		def initialize(selector, block)
			@selector = selector
			@input, @output = ::IO.pipe
			
			@fiber = Fiber.new do
				while true
					@selector.io_wait(@fiber, @input, READABLE)
					block.call(@input.read_nonblock(1)&.ord)
				end
			end
			
			@fiber.transfer
		end
		
		# Send a sigle byte interrupt.
		def signal(event = 0)
			@output.write(event.chr)
			@output.flush
		end
		
		def close
			@input.close
			@output.close
		end
	end
end
