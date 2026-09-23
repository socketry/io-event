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
		
		it "terminates flush when the newest entry is removed out of band" do
			sequence = []
			count = 0
			
			# This fiber re-queues itself on every iteration, so the queue is never empty:
			busy = Fiber.new do
				while true
					count += 1
					selector.push(busy)
					selector.transfer
				end
			end
			
			yielding = Fiber.new do
				selector.push(busy)
				
				# Simulate a stale internal entry, e.g. from `Fiber::Scheduler#unblock` racing a timeout:
				selector.push(yielding)
				
				# Yielding adds a stack-allocated entry to the head of the queue, which is removed when the fiber is resumed:
				sequence << :yield
				selector.yield
				sequence << :resumed
			end
			
			selector.push(yielding)
			selector.select(0)
			expect(sequence).to be == [:yield]
			
			# The stale entry resumes `yielding`, which removes the newest entry from the queue while we are still flushing. The re-queued `busy` must be deferred to the next flush:
			selector.select(0)
			expect(sequence).to be == [:yield, :resumed]
			expect(count).to be == 1
			
			# Only `busy` remains in the queue, so it runs exactly once more:
			previous = count
			selector.select(0)
			expect(count).to be == previous + 1
		end
		
		it "defers new entries when an earlier entry is removed out of band" do
			sequence = []
			yielding = nil
			added = Fiber.new{sequence << :added}
			
			remover = Fiber.new do
				sequence << :remover
				selector.push(added)
				yielding.transfer
			end
			
			yielding = Fiber.new do
				selector.push(remover)
				selector.yield
				sequence << :resumed
			end
			
			selector.push(yielding)
			selector.select(0)
			selector.push(Fiber.new{sequence << :tail})
			
			# The initial queue is [remover, yielding, tail]. Resuming yielding
			# removes its entry, but must not bring added into this flush.
			selector.select(0)
			expect(sequence).to be == [:remover, :resumed, :tail]
			
			selector.select(0)
			expect(sequence).to be == [:remover, :resumed, :tail, :added]
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
	
	with "#ready?" do
		it "ignores flush placeholders but sees entries appended after them" do
			states = []
			
			selector.push(Fiber.new do
				states << selector.ready?
				selector.push(Fiber.new{states << selector.ready?})
				states << selector.ready?
			end)
			
			expect(selector).to be(:ready?)
			selector.select(0)
			expect(states).to be == [false, true]
			expect(selector).to be(:ready?)
			
			selector.select(0)
			expect(states).to be == [false, true, false]
			expect(selector).not.to be(:ready?)
		end
	end
	
	with "#select" do
		it "preserves queued entries when a fiber raises during flush" do
			sequence = []
			
			selector.push(Fiber.new do
				selector.push(Fiber.new{sequence << :added})
				raise Interrupt, "interrupted flush"
			end)
			selector.push(Fiber.new{sequence << :remaining})
			
			expect{selector.select(0)}.to raise_exception(Interrupt, message: be == "interrupted flush")
			expect(selector).to be(:ready?)
			
			GC.start
			selector.select(0)
			expect(sequence).to be == [:remaining, :added]
			expect(selector).not.to be(:ready?)
		end
		
		it "leaves an empty queue when the only fiber raises during flush" do
			selector.push(Fiber.new{raise "interrupted flush"})
			
			expect{selector.select(0)}.to raise_exception(RuntimeError, message: be == "interrupted flush")
			expect(selector).not.to be(:ready?)
			
			sequence = []
			selector.push(Fiber.new{sequence << :resumed})
			selector.select(0)
			expect(sequence).to be == [:resumed]
			expect(selector).not.to be(:ready?)
		end
		
		it "supports garbage collection while flushing the queue" do
			sequence = []
			
			selector.push(Fiber.new do
				if GC.respond_to?(:verify_compaction_references)
					GC.verify_compaction_references(double_heap: true, toward: :empty)
				else
					GC.start
				end
				sequence << :collected
			end)
			selector.push(Fiber.new{sequence << :remaining})
			
			selector.select(0)
			expect(sequence).to be == [:collected, :remaining]
			expect(selector).not.to be(:ready?)
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
		
		unless klass == IO::Event::Selector::Select
			it "uses each nested flush's own boundary" do
				sequence = []
				event_selector = selector
				callback = Object.new
				callback.define_singleton_method(:alive?){true}
				callback.define_singleton_method(:transfer) do
					sequence << :outer
					event_selector.push(Fiber.new do
						sequence << :added
						event_selector.push(Fiber.new{sequence << :deferred})
					end)
					event_selector.select(0)
					sequence << :returned
				end
				
				selector.push(callback)
				selector.push(Fiber.new{sequence << :inner})
				selector.select(0)
				expect(sequence).to be == [:outer, :inner, :added, :returned]
				expect(selector).to be(:ready?)
				
				selector.select(0)
				expect(sequence).to be == [:outer, :inner, :added, :returned, :deferred]
				expect(selector).not.to be(:ready?)
			end
			
			it "skips multiple outer placeholders without removing them" do
				sequence = []
				event_selector = selector
				middle = Object.new
				middle.define_singleton_method(:alive?){true}
				middle.define_singleton_method(:transfer) do
					sequence << :middle
					event_selector.push(Fiber.new do
						sequence << :inner
						event_selector.push(Fiber.new{sequence << :deferred})
					end)
					# The inner flush must cross both the outer and middle placeholders:
					event_selector.select(0)
					sequence << :middle_returned
				end
				
				outer = Object.new
				outer.define_singleton_method(:alive?){true}
				outer.define_singleton_method(:transfer) do
					sequence << :outer
					event_selector.push(middle)
					event_selector.select(0)
					sequence << :outer_returned
				end
				
				selector.push(outer)
				selector.select(0)
				expect(sequence).to be == [:outer, :middle, :inner, :middle_returned, :outer_returned]
				expect(selector).to be(:ready?)
				
				selector.select(0)
				expect(sequence).to be == [:outer, :middle, :inner, :middle_returned, :outer_returned, :deferred]
				expect(selector).not.to be(:ready?)
			end
			
			it "preserves the outer placeholder when a nested flush raises" do
				sequence = []
				event_selector = selector
				callback = Object.new
				callback.define_singleton_method(:alive?){true}
				callback.define_singleton_method(:transfer) do
					sequence << :outer
					event_selector.push(Fiber.new do
						sequence << :raised
						event_selector.push(Fiber.new{sequence << :deferred})
						raise "interrupted nested flush"
					end)
					event_selector.push(Fiber.new{sequence << :remaining})
					
					begin
						event_selector.select(0)
					rescue RuntimeError
						sequence << :rescued
					end
					
					GC.start
				end
				
				selector.push(callback)
				selector.push(Fiber.new{sequence << :initial})
				selector.select(0)
				expect(sequence).to be == [:outer, :initial, :raised, :rescued]
				expect(selector).to be(:ready?)
				
				selector.select(0)
				expect(sequence).to be == [:outer, :initial, :raised, :rescued, :remaining, :deferred]
				expect(selector).not.to be(:ready?)
			end
			
			it "ignores nested placeholders when checking readiness" do
				states = []
				event_selector = selector
				callback = Object.new
				callback.define_singleton_method(:alive?){true}
				callback.define_singleton_method(:transfer){event_selector.select(0)}
				
				selector.push(callback)
				selector.push(Fiber.new do
					states << selector.ready?
					selector.push(Fiber.new{})
					states << selector.ready?
				end)
				
				selector.select(0)
				expect(states).to be == [false, true]
				selector.select(0)
				expect(selector).not.to be(:ready?)
			end
		end
	end
end
