# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/selector"

describe IO::Event::Selector::Select do
	before do
		@selector = subject.new(Fiber.current)
	end
	
	after do
		@selector&.close
	end
	
	attr :selector
	
	with "#ready?" do
		it "reports whether a fiber is ready" do
			fiber = Fiber.new{}
			
			expect(selector).not.to be(:ready?)
			
			selector.push(fiber)
			
			expect(selector).to be(:ready?)
			
			selector.select(0)
			
			expect(selector).not.to be(:ready?)
		end
	end
	
	with "Waiter" do
		it "can report whether the fiber is alive" do
			fiber = Fiber.new{}
			waiter = subject::Waiter.new(fiber, IO::READABLE, nil)
			
			expect(waiter).to be(:alive?)
			
			fiber.transfer
			
			expect(waiter).not.to be(:alive?)
		end
		
		it "clears dead fibers while dispatching" do
			fiber = Fiber.new{}
			fiber.transfer
			
			waiter = subject::Waiter.new(fiber, IO::READABLE, nil)
			waiter.dispatch(IO::READABLE){}
			
			expect(waiter.fiber).to be_nil
		end
	end
	
	with "#io_select" do
		it "delegates to IO.select from a worker thread" do
			input, output = IO.pipe
			output.write(".")
			
			readable, = selector.io_select([input], nil, nil, 0)
			
			expect(readable).to be == [input]
		ensure
			input&.close
			output&.close
		end
	end
	
	with "#io_read" do
		it "returns zero at EOF" do
			input, output = IO.pipe
			output.close
			
			buffer = IO::Buffer.new(64)
			
			expect(selector.io_read(Fiber.current, input, buffer, 1)).to be == 0
		ensure
			input&.close
		end
	end
	
	with "#select" do
		it "dispatches priority events" do
			input, output = IO.pipe
			events = nil
			
			fiber = Fiber.new do
				events = selector.io_wait(Fiber.current, input, IO::PRIORITY)
			end
			
			fiber.transfer
			
			IO.singleton_class.class_eval do
				alias_method :select_without_io_event_priority_test, :select
				
				def select(readable, writable, priority, duration = nil)
					[nil, nil, priority]
				end
			end
			
			expect(selector.select(0)).to be == 1
			expect(events).to be == IO::PRIORITY
		ensure
			IO.singleton_class.class_eval do
				if method_defined?(:select_without_io_event_priority_test)
					alias_method :select, :select_without_io_event_priority_test
					remove_method :select_without_io_event_priority_test
				end
			end
			
			input&.close
			output&.close
		end
	end
end
