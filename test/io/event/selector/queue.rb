# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

Queue = Sus::Shared("queue") do
	with "#transfer" do
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
	
	with "#push" do
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
	
	with "#raise" do
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
	
	with "#resume" do
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
	
	with "#yield" do
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
		before do
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		after do
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like Queue
	end
end
