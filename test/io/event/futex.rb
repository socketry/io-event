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
	
	with "buffer lifetime" do
		it "locks the allocation until closed" do
			instance = subject.new(buffer)
			expect(buffer).to be(:locked?)
			expect{buffer.free}.to raise_exception(IO::Buffer::LockedError)
			expect{buffer.resize(16)}.to raise_exception(IO::Buffer::LockedError)
			instance.close
			expect(instance).to be(:closed?)
			expect(buffer).not.to be(:locked?)
			buffer.free
		end
		
		it "owns one independent lock per futex" do
			first = subject.new(buffer)
			second = subject.new(buffer, offset: 4)
			first.close
			first.close
			expect(buffer).to be(:locked?)
			expect(second.increment).to be == 1
			second.close
			expect(buffer).not.to be(:locked?)
		end
		
		it "locks the root allocation when bound to a slice" do
			instance = subject.new(buffer.slice(4, 4))
			expect{buffer.free}.to raise_exception(IO::Buffer::LockedError)
			instance.close
			expect(buffer).not.to be(:locked?)
		end
		
		it "does not leak a lock when initialization fails" do
			expect{subject.new(buffer, offset: 1)}.to raise_exception(ArgumentError)
			expect{subject.new(buffer, offset: 8)}.to raise_exception(RangeError)
			expect(buffer).not.to be(:locked?)
		end
		
		it "does not allow reinitialization or copying" do
			instance = subject.new(buffer)
			expect{instance.send(:initialize, buffer)}.to raise_exception(RuntimeError)
			expect{instance.dup}.to raise_exception(TypeError)
			expect{instance.clone}.to raise_exception(TypeError)
			instance.close
			expect(buffer).not.to be(:locked?)
		end
		
		it "does not unlock the original when rejected copies are collected" do
			instance = subject.new(buffer)
			Thread.new do
				expect{instance.dup}.to raise_exception(TypeError)
				expect{instance.clone}.to raise_exception(TypeError)
			end.join
			3.times{GC.start(full_mark: true, immediate_sweep: true)}
			expect(buffer).to be(:locked?)
			expect(instance.increment).to be == 1
			instance.close
		end
		
		it "releases locks when futexes are collected" do
			# A separate thread removes conservative C-stack references before GC:
			Thread.new{subject.new(buffer); nil}.join
			10.times do
				GC.start(full_mark: true, immediate_sweep: true)
				break unless buffer.locked?
			end
			expect(buffer).not.to be(:locked?)
		end
		
		it "does not unlock another futex when a closed instance is collected" do
			instance = subject.new(buffer)
			Thread.new{subject.new(buffer, offset: 4).close}.join
			3.times{GC.start(full_mark: true, immediate_sweep: true)}
			expect(buffer).to be(:locked?)
			expect(instance.increment).to be == 1
			instance.close
			expect(buffer).not.to be(:locked?)
		end
		
		it "retains the buffer across compaction" do
			instance = subject.new(buffer)
			GC.verify_compaction_references(double_heap: true, toward: :empty)
			expect(instance.increment).to be == 1
			instance.close
			expect(buffer).not.to be(:locked?)
		end
	end
	
	with "closed or uninitialized futexes" do
		[
			[:value], [:value=, 1], [:increment], [:decrement],
			[:compare_exchange, 0, 1], [:wake], [:wake, 0], [:signal], [:signal, 0], [:wait, 0]
		].each do |arguments|
			it "rejects #{arguments.inspect} after close", unique: "closed #{arguments.inspect}" do
				futex.close
				expect{futex.public_send(*arguments)}.to raise_exception(IOError)
			end
			
			it "rejects #{arguments.inspect} before initialization", unique: "uninitialized #{arguments.inspect}" do
				expect{subject.allocate.public_send(*arguments)}.to raise_exception(IOError)
			end
		end
	end
	
	with "argument coercion" do
		it "rechecks the futex after numeric conversion closes it" do
			instance = futex
			value = Object.new
			value.define_singleton_method(:to_int) do
				instance.close
				1
			end
			expect{instance.value = value}.to raise_exception(IOError)
		end
		
		it "validates the wake count before changing the value" do
			expect{futex.signal(-1)}.to raise_exception(ArgumentError)
			expect(futex.value).to be == 0
		end
	end
	
	with "#value" do
		it "stores and loads the value atomically" do
			futex.value = 42
			expect(futex.value).to be == 42
		end
	end
	
	with "zero-count notifications" do
		[:wake, :signal].each do |operation|
			it "leaves the word and waiters unchanged for #{operation}(0)", unique: operation.to_s do
				futex.value = 7
				thread = Thread.new{futex.wait(7)}
				Thread.pass while thread.status == "run"
				
				expect(futex.public_send(operation, 0)).to be == (operation == :wake ? 0 : 7)
				expect(futex.value).to be == 7
				expect(thread.join(0.02)).to be_nil
			ensure
				thread&.kill&.join
			end
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
		it "requires an explicit expected value" do
			expect{futex.wait}.to raise_exception(ArgumentError)
		end
		
		it "cannot be closed during a blocking wait" do
			instance = futex
			thread = Thread.new{instance.wait(0)}
			Thread.pass while thread.status == "run"
			expect{instance.close}.to raise_exception(IOError)
			instance.signal
			thread.join
			instance.close
			expect(buffer).not.to be(:locked?)
		ensure
			thread&.kill&.join
		end
		
		it "releases a blocking wait when its thread is interrupted" do
			instance = futex
			thread = Thread.new{instance.wait(0)}
			Thread.pass while thread.status == "run"
			thread.kill.join
			instance.close
			expect(buffer).not.to be(:locked?)
		ensure
			thread&.kill&.join
		end
		
		it "cannot be closed while an asynchronous wait is pending" do
			selector = uring_selector
			fiber = Fiber.new{selector.futex_wait(Fiber.current, futex, 0)}
			fiber.transfer
			expect{futex.close}.to raise_exception(IOError)
			futex.signal
			10.times do
				selector.select(0.1)
				break unless fiber.alive?
			end
			expect(fiber).not.to be(:alive?)
			futex.close
			expect(buffer).not.to be(:locked?)
		ensure
			selector&.close
		end
		
		it "drains cancellation before allowing close" do
			selector = uring_selector
			error = RuntimeError.new("cancel futex")
			caught = nil
			fiber = Fiber.new do
				selector.futex_wait(Fiber.current, futex, 0)
			rescue RuntimeError => exception
				caught = exception
			end
			fiber.transfer
			selector.select(0)
			fiber.raise(error)
			expect{futex.close}.to raise_exception(IOError) if fiber.alive?
			10.times do
				selector.select(0.1)
				break unless fiber.alive?
			end
			expect(caught).to be_equal(error)
			expect(fiber).not.to be(:alive?)
			futex.close
			expect(buffer).not.to be(:locked?)
			# Drain the cancellation CQE as well as the original operation:
			selector.select(0)
		ensure
			selector&.close
		end
		
		it "does not report an out-of-band resume as a notification" do
			selector = uring_selector
			result = :pending
			fiber = Fiber.new{result = selector.futex_wait(Fiber.current, futex, 0)}
			fiber.transfer
			selector.select(0)
			fiber.transfer
			10.times do
				selector.select(0.1)
				break unless fiber.alive?
			end
			expect(result).to be == false
			futex.close
		ensure
			selector&.close
		end
		
		it "keeps the selector usable after invalid arguments" do
			selector = uring_selector
			fiber = Fiber.new do
				expect{selector.futex_wait(Fiber.current, futex, Object.new)}.to raise_exception(TypeError)
				expect{selector.futex_wait(Fiber.current, Object.new, 0)}.to raise_exception(TypeError)
				GC.start
				futex.value = 1
				expect(selector.futex_wait(Fiber.current, futex, 0)).to be == false
			end
			fiber.transfer
			10.times do
				selector.select(0.1)
				break unless fiber.alive?
			end
			expect(fiber).not.to be(:alive?)
			futex.close
		ensure
			selector&.close
		end
		
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
		
		it "does not wait for a notification published after the snapshot" do
			expected = futex.value
			futex.signal
			expect(futex.wait(expected)).to be == false
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
			it "releases earlier entries when a later entry is invalid" do
				expect{subject.wait_any([[futex, 0], [Object.new, 0]])}.to raise_exception(TypeError)
				futex.close
				expect(buffer).not.to be(:locked?)
			end
			
			it "protects every futex during a blocking vector wait" do
				first = subject.new(buffer)
				second = subject.new(buffer, offset: 4)
				thread = Thread.new{subject.wait_any([[first, 0], [second, 0]])}
				Thread.pass while thread.status == "run"
				expect{first.close}.to raise_exception(IOError)
				expect{second.close}.to raise_exception(IOError)
				thread.kill.join
				first.close
				second.close
				expect(buffer).not.to be(:locked?)
			ensure
				thread&.kill&.join
			end
			
			it "retains its own entries until vector cancellation completes" do
				selector = waitv_selector
				first = subject.new(buffer)
				second = subject.new(buffer, offset: 4)
				entries = [[first, 0], [second, 0]]
				caught = nil
				error = RuntimeError.new("cancel vector")
				fiber = Fiber.new do
					selector.futex_waitv(Fiber.current, entries)
				rescue RuntimeError => exception
					caught = exception
				end
				fiber.transfer
				entries.clear
				GC.verify_compaction_references(double_heap: true, toward: :empty)
				expect{first.close}.to raise_exception(IOError)
				expect{second.close}.to raise_exception(IOError)
				selector.select(0)
				fiber.raise(error)
				10.times do
					selector.select(0.1)
					break unless fiber.alive?
				end
				expect(caught).to be_equal(error)
				expect(fiber).not.to be(:alive?)
				first.close
				second.close
				expect(buffer).not.to be(:locked?)
				selector.select(0)
			ensure
				selector&.close
			end
			
			it "keeps the selector usable after partial vector setup fails" do
				selector = waitv_selector
				fiber = Fiber.new do
					expect{selector.futex_waitv(Fiber.current, [[futex, 0], [Object.new, 0]])}.to raise_exception(TypeError)
					futex.close
				end
				fiber.transfer
				GC.start
				selector.select(0)
				expect(buffer).not.to be(:locked?)
			ensure
				selector&.close
			end
			
			it "returns nil for an out-of-band vector resume" do
				selector = waitv_selector
				first = subject.new(buffer)
				second = subject.new(buffer, offset: 4)
				result = :pending
				fiber = Fiber.new{result = selector.futex_waitv(Fiber.current, [[first, 0], [second, 0]])}
				fiber.transfer
				selector.select(0)
				fiber.transfer
				10.times do
					selector.select(0.1)
					break unless fiber.alive?
				end
				expect(result).to be_nil
				expect(fiber).not.to be(:alive?)
				first.close
				second.close
				expect(buffer).not.to be(:locked?)
				selector.select(0)
			ensure
				selector&.close
			end
			
			it "returns index zero for a single-entry vector notification" do
				selector = waitv_selector
				result = nil
				fiber = Fiber.new{result = selector.futex_waitv(Fiber.current, [[futex, 0]])}
				fiber.transfer
				thread = Thread.new do
					sleep 0.01
					futex.signal
				end
				selector.select(1)
				thread.join
				expect(result).to be == 0
				futex.close
				expect(buffer).not.to be(:locked?)
			ensure
				selector&.close
				thread&.join
			end
			
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
