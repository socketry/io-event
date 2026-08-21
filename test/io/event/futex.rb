# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"

return unless defined?(IO::Event::Futex)

describe IO::Event::Futex do
	let(:buffer) {IO::Buffer.new(8)}
	let(:futex) {subject.new(buffer)}
	
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
		it "requires scheduler support" do
			expect do
				futex.wait(0)
			end.to raise_exception(NotImplementedError)
		end
		
		it "waits asynchronously for a signal" do
			selector = IO::Event::Selector::URing.new(Fiber.current)
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
		
		it "does not wait when the value has changed" do
			selector = IO::Event::Selector::URing.new(Fiber.current)
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
end
