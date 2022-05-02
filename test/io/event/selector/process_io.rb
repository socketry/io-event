# Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, selector to the following conditions:
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

require 'io/event'
require 'io/event/selector'
require 'io/event/debug/selector'

require 'socket'
require 'fiber'

ProcessIO = Sus::Shared("process io") do
	it "can wait for a process which has terminated already" do
		result = nil
		
		fiber = Fiber.new do
			input, output = IO.pipe
			
			# For some reason, sleep 0.1 here is very unreliable...?
			pid = Process.spawn("true", out: output)
			output.close
			
			# Internally, this should generate POLLHUP, which is what we want to test:
			expect(selector.io_wait(Fiber.current, input, IO::READABLE)).to be == IO::READABLE
			input.close
			
			_, result = Process.wait2(pid)
		end
		
		fiber.transfer
		selector.select(1)
		
		expect(result.success?).to be == true
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		def before
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		def after
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like ProcessIO
	end
end
