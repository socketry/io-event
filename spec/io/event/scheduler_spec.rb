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

require_relative 'scheduler'

RSpec.shared_examples_for IO::Event::Scheduler do
	subject(:scheduler) {IO::Event::Scheduler.new(selector)}
	
	around do |example|
		thread = Thread.new do
			Fiber.set_scheduler(scheduler)
			example.run
		end
		
		thread.join
	end
	
	it 'can run several fibers' do
		sum = 0
		
		fibers = 3.times.map do |i|
			Fiber.schedule{sleep 0.001; sum += i}
		end
		
		subject.run
		
		expect(sum).to be == 3
	end
	
	it 'can join threads' do
		Fiber.schedule do
			1000.times do
				thread = ::Thread.new do
					sleep(0.001)
				end
				
				thread.join(0.001)
			ensure
				thread&.join
			end
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	RSpec.describe(klass) do
		let(:loop) {Fiber.current}
		let(:selector){described_class.new(loop)}
		
		it_behaves_like IO::Event::Scheduler
	end
end
