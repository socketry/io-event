# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/test_scheduler"

return unless defined?(IO::Event::Futex)

describe IO::Event::Futex do
	let(:buffer) {IO::Buffer.new(8)}
	let(:futex) {subject.new(buffer)}
	let(:uring_selector) do
		unless defined?(IO::Event::Selector::URing)
			skip "io_uring is not available"
		end
		
		selector = IO::Event::Selector::URing.new(Fiber.current)
		unless selector.respond_to?(:futex_wait)
			selector.close
			skip "io_uring futex operations are not available"
		end
		
		selector
	end
	let(:waitv_selector) do
		selector = uring_selector
		unless selector.respond_to?(:futex_waitv)
			selector.close
			skip "io_uring futex waitv operations are not available"
		end
		
		selector
	end
	
	with "#value" do
		it "stores and loads the value atomically" do
			futex.value = 42
			expect(futex.value).to be == 42
		end
	end
	
	with "#increment" do
		it "increments the value" do
			expect(futex.increment).to be == 1
			expect(futex.increment(2)).to be == 3
		end
	end
	
	with "#decrement" do
		it "decrements the value" do
			futex.value = 3
			
			expect(futex.decrement).to be == 2
			expect(futex.decrement(2)).to be == 0
		end
	end
	
	with "#compare_exchange" do
		it "exchanges a matching value" do
			futex.value = 2
			
			expect(futex.compare_exchange(2, 1)).to be == true
			expect(futex.value).to be == 1
		end
		
		it "does not exchange a different value" do
			futex.value = 2
			
			expect(futex.compare_exchange(1, 0)).to be == false
			expect(futex.value).to be == 2
		end
	end
	
	with "offset:" do
		it "can address independent words in one buffer" do
			first = subject.new(buffer, offset: 0)
			second = subject.new(buffer, offset: 4)
			
			first.value = 1
			second.value = 2
			
			expect(first.value).to be == 1
			expect(second.value).to be == 2
		end
		
		it "rejects unaligned offsets" do
			expect do
				subject.new(buffer, offset: 1)
			end.to raise_exception(ArgumentError)
		end
		
		it "rejects offsets outside the buffer" do
			expect do
				subject.new(buffer, offset: 8)
			end.to raise_exception(RangeError)
		end
	end
	
	with "#wait" do
		it "waits without blocking other Ruby threads when no scheduler is installed" do
			thread = Thread.new do
				sleep 0.01
				futex.signal
			end
			
			expect(futex.wait(0)).to be == true
			expect(futex.value).to be == 1
		ensure
			thread&.join
		end
		
		it "does not wait without a scheduler when the value has changed" do
			futex.value = 1
			expect(futex.wait(0)).to be == false
		end
		
		it "waits asynchronously for a signal" do
			selector = uring_selector
			result = nil
			
			fiber = Fiber.new do
				result = selector.futex_wait(Fiber.current, futex, 0)
			end
			fiber.transfer
			
			thread = Thread.new do
				sleep 0.01
				futex.signal
			end
			
			selector.select(1)
			thread.join
			
			expect(result).to be == true
			expect(futex.value).to be == 1
		ensure
			selector&.close
			thread&.join
		end
		
		it "uses the current scheduler" do
			selector = uring_selector
			scheduler = IO::Event::TestScheduler.new(selector: selector)
			result = nil
			
			Fiber.set_scheduler(scheduler)
			Fiber.schedule do
				result = futex.wait(0)
			end
			
			thread = Thread.new do
				sleep 0.01
				futex.signal
			end
			
			scheduler.run
			
			expect(result).to be == true
		ensure
			Fiber.set_scheduler(nil)
			thread&.join
		end
		
		it "does not wait when the value has changed" do
			selector = uring_selector
			futex.value = 1
			result = nil
			
			fiber = Fiber.new do
				result = selector.futex_wait(Fiber.current, futex, 0)
			end
			fiber.transfer
			selector.select(1)
			
			expect(result).to be == false
		ensure
			selector&.close
		end
	end
	
	if IO::Event::Futex.respond_to?(:wait_any)
		with ".wait_any" do
			it "exposes the maximum number of wait entries" do
				expect(subject::WAITV_LIMIT).to be == 128
			end
			
			it "rejects more than the maximum number of wait entries" do
				entries = Array.new(subject::WAITV_LIMIT + 1){[futex, 0]}
				
				expect do
					subject.wait_any(entries)
				end.to raise_exception(ArgumentError)
			end
			
			it "waits without blocking other Ruby threads when no scheduler is installed" do
				first = subject.new(buffer, offset: 0)
				second = subject.new(buffer, offset: 4)
				
				thread = Thread.new do
					sleep 0.01
					second.signal
				end
				
				expect(subject.wait_any([[first, 0], [second, 0]])).to be == 1
			ensure
				thread&.join
			end
			
			it "does not wait without a scheduler when a value has changed" do
				first = subject.new(buffer, offset: 0)
				second = subject.new(buffer, offset: 4)
				second.value = 1
				
				expect(subject.wait_any([[first, 0], [second, 0]])).to be_nil
			end
			
			it "waits asynchronously for any futex to be signalled" do
				selector = waitv_selector
				
				first = subject.new(buffer, offset: 0)
				second = subject.new(buffer, offset: 4)
				result = nil
				
				fiber = Fiber.new do
					result = selector.futex_waitv(Fiber.current, [[first, 0], [second, 0]])
				end
				fiber.transfer
				
				thread = Thread.new do
					sleep 0.01
					second.signal
				end
				
				selector.select(1)
				thread.join
				
				expect(result).to be == 1
			ensure
				selector&.close
				thread&.join
			end
			
			it "uses the current scheduler" do
				selector = waitv_selector
				
				scheduler = IO::Event::TestScheduler.new(selector: selector)
				first = subject.new(buffer, offset: 0)
				second = subject.new(buffer, offset: 4)
				result = nil
				
				Fiber.set_scheduler(scheduler)
				Fiber.schedule do
					result = subject.wait_any([[first, 0], [second, 0]])
				end
				
				thread = Thread.new do
					sleep 0.01
					second.signal
				end
				
				scheduler.run
				
				expect(result).to be == 1
			ensure
				Fiber.set_scheduler(nil)
				thread&.join
			end
			
			it "returns nil when a value has changed" do
				selector = waitv_selector
				
				first = subject.new(buffer, offset: 0)
				second = subject.new(buffer, offset: 4)
				second.value = 1
				result = :waiting
				
				fiber = Fiber.new do
					result = selector.futex_waitv(Fiber.current, [[first, 0], [second, 0]])
				end
				fiber.transfer
				selector.select(1)
				
				expect(result).to be_nil
			ensure
				selector&.close
			end
		end
	end
end
