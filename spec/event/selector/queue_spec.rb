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

require 'event'
require 'event/selector'
require 'socket'

RSpec.shared_examples_for "queue" do
	let!(:loop) {Fiber.current}
	subject{described_class.new(loop)}
	
	after do
		subject.close
	end
	
	describe '#transfer' do
		it "can transfer back to event loop" do
			sequence = []
			
			fiber = Fiber.new do
				while true
					sequence << :transfer
					subject.transfer
				end
			end
			
			subject.push(fiber)
			sequence << :select
			subject.select(0)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:select, :transfer, :select]
		end
	end
	
	describe '#push' do
		it "can push fiber into queue" do
			sequence = []
			
			fiber = Fiber.new do
				sequence << :executed
			end
			
			subject.push(fiber)
			subject.select(0)
			
			expect(sequence).to be == [:executed]
		end
		
		it "can push non-fiber object into queue" do
			object = double
			
			expect(object).to receive(:alive?).and_return(true)
			expect(object).to receive(:transfer)
			
			subject.push(object)
			subject.select(0)
		end
		
		it "defers push during push to next iteration" do
			sequence = []
			
			fiber = Fiber.new do
				sequence << :yield
				subject.yield
				sequence << :resume
			end
			
			subject.push(fiber)
			sequence << :select
			subject.select(0)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:select, :yield, :select, :resume]
		end
	end
	
	describe '#raise' do
		it "can raise exception on fiber" do
			sequence = []
			
			fiber = Fiber.new do
				begin
					subject.yield
				rescue
					sequence << :rescue
				end
			end
			
			subject.push(fiber)
			subject.select(0)
			
			sequence << :raise
			subject.raise(fiber, "Boom")
			
			expect(sequence).to be == [:raise, :rescue]
		end
	end
	
	describe '#resume' do
		it "can resume a fiber for execution from the main fiber" do
			sequence = []
			
			fiber = Fiber.new do |argument|
				sequence << argument
			end
			
			subject.resume(fiber, :resumed)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:resumed, :select]
		end
		
		it "can resume a fiber for execution from a nested fiber" do
			sequence = []
			
			child = Fiber.new do |argument|
				sequence << argument
			end
			
			parent = Fiber.new do |argument|
				sequence << argument
				subject.resume(child, :child)
				sequence << :parent
			end
			
			subject.resume(parent, :resumed)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:resumed, :child, :select, :parent]
		end
	end
	
	describe '#yield' do
		it "can yield to the scheduler and later resume execution" do
			sequence = []
			
			fiber = Fiber.new do |argument|
				sequence << :yield
				subject.yield
				sequence << :resumed
			end
			
			subject.resume(fiber)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:yield, :select, :resumed]
		end
		
		it "can yield from resumed fiber" do
			sequence = []
			
			child = Fiber.new do |argument|
				sequence << :yield
				subject.yield
				sequence << :resumed
			end
			
			parent = Fiber.new do
				child.resume
			end
			
			subject.resume(parent)
			sequence << :select
			subject.select(0)
			
			expect(sequence).to be == [:yield, :select, :resumed]
		end
	end
end

Event::Selector.constants.each do |name|
	selector = Event::Selector.const_get(name)
	
	RSpec.describe selector do
		it_behaves_like "queue"
	end
end
