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

require_relative '../../../environment'

require 'io/event'
require 'io/event/selector'
require 'socket'

FileIO = Sus::Shared("file io") do
	with 'a file' do
		let(:file) {File.open(__FILE__)}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		it "can read using a buffer" do
			events = Array.new
			
			fiber = Fiber.new do
				events << :wait_readable
				
				selector.io_wait(Fiber.current, file, IO::READABLE)
				
				events << :readable
			end
			
			events << :transfer
			fiber.transfer
			events << :transfer
			fiber.transfer
			
			expect(events).to be == [:transfer, :wait_readable, :transfer, :readable]
		end
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
		
		it_behaves_like FileIO
	end
end
