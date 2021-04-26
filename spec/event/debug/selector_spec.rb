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

require 'event/debug/selector'
require_relative '../selector_examples'

RSpec.describe Event::Debug::Selector do
	let!(:loop) {Fiber.current}
	subject {described_class.new(Event::Backend::Select.new(loop))}
	
	describe '#io_wait' do
		let(:events) {Array.new}
		let(:sockets) {UNIXSocket.pair}
		let(:local) {sockets.first}
		let(:remote) {sockets.last}
		
		it "cannot have two fibers reading from the same io" do
			fiber1 = Fiber.new do
				events << :wait_readable1
				subject.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable1
			rescue
				events << :error1
			end
			
			fiber2 = Fiber.new do
				events << :wait_readable2
				subject.io_wait(Fiber.current, local, IO::READABLE)
				events << :readable2
			rescue
				events << :error2
			end
			
			events << :transfer
			fiber1.transfer
			fiber2.transfer
			
			remote.puts "Hello World"
			events << :select
			subject.select(1)
			
			expect(events).to be == [
				:transfer, :wait_readable1, :wait_readable2,
				:error2,
				:select, :readable1
			]
		end
	end
end