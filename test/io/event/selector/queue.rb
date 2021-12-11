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

Queue = Sus::Shared("queue") do
	with '#transfer' do
		it "can transfer back to event loop" do
			sequence = []
			
			fiber = Fiber.new do
				while true
					sequence << :transfer
					selector.transfer
				end
			end
			
			selector.push(fiber)
			sequence << :select
			selector.select(0)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:select, :transfer, :select]
		end
	end
	
	with '#push' do
		it "can push fiber into queue" do
			sequence = []
			
			fiber = Fiber.new do
				sequence << :executed
			end
			
			selector.push(fiber)
			selector.select(0)
			
			expect(sequence).to be == [:executed]
		end
		
		it "can push non-fiber object into queue" do
			object = Object.new
			
			def object.alive?
				true
			end
			
			def object.transfer
			end
			
			selector.push(object)
			selector.select(0)
		end
		
		it "defers push during push to next iteration" do
			sequence = []
			
			fiber = Fiber.new do
				sequence << :yield
				selector.yield
				sequence << :resume
			end
			
			selector.push(fiber)
			sequence << :select
			selector.select(0)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:select, :yield, :select, :resume]
		end
		
		it "can push a fiber into the queue while processing queue" do
			sequence = []
			
			second = Fiber.new do
				sequence << :second
			end
			
			first = Fiber.new do
				sequence << :first
				selector.push(second)
			end
			
			selector.push(first)
			
			selector.select(0)
			expect(sequence).to be == [:first]
			
			selector.select(0)
			expect(sequence).to be == [:first, :second]
		end
	end
	
	with '#raise' do
		it "can raise exception on fiber" do
			sequence = []
			
			fiber = Fiber.new do
				begin
					selector.yield
				rescue
					sequence << :rescue
				end
			end
			
			selector.push(fiber)
			selector.select(0)
			
			sequence << :raise
			selector.raise(fiber, "Boom")
			
			expect(sequence).to be == [:raise, :rescue]
		end
	end
	
	with '#resume' do
		it "can resume a fiber for execution from the main fiber" do
			sequence = []
			
			fiber = Fiber.new do |argument|
				sequence << argument
			end
			
			selector.resume(fiber, :resumed)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:resumed, :select]
		end
		
		it "can resume a fiber for execution from a nested fiber" do
			sequence = []
			
			child = Fiber.new do |argument|
				sequence << argument
			end
			
			parent = Fiber.new do |argument|
				sequence << argument
				selector.resume(child, :child)
				sequence << :parent
			end
			
			selector.resume(parent, :resumed)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:resumed, :child, :select, :parent]
		end
	end
	
	with '#yield' do
		it "can yield to the scheduler and later resume execution" do
			sequence = []
			
			fiber = Fiber.new do |argument|
				sequence << :yield
				selector.yield
				sequence << :resumed
			end
			
			selector.resume(fiber)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:yield, :select, :resumed]
		end
		
		it "can yield from resumed fiber" do
			sequence = []
			
			child = Fiber.new do |argument|
				sequence << :yield
				selector.yield
				sequence << :resumed
			end
			
			parent = Fiber.new do
				child.resume
			end
			
			selector.resume(parent)
			sequence << :select
			selector.select(0)
			
			expect(sequence).to be == [:yield, :select, :resumed]
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
		
		it_behaves_like Queue
	end
end
