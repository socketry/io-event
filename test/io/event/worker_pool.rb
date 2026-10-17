# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.
# Copyright, 2026, by Fletcher Dares.

require "io/event"
require "io/event/test_scheduler"

return unless defined?(IO::Event::WorkerPool)

# Simulate a finalizer performing a blocking operation before the scheduler suspends the fiber. The operation must run synchronously to avoid consuming the outer operation's wakeup.
class ReentrantBlockScheduler < IO::Event::TestScheduler
	attr_reader :nested_result, :blocking_states
	
	def initialize(...)
		super
		@blocking_states = []
	end
	
	def block(blocker, timeout = nil)
		@blocking_states << Fiber.current.blocking? if blocker.is_a?(IO::Event::WorkerPool)
		
		# Simulate a finalizer that performs an offloaded blocking operation, exactly once:
		if blocker.is_a?(IO::Event::WorkerPool) and !@reentered
			@reentered = true
			@nested_result = IO::Event::WorkerPool.busy(duration: 0.01)
		end
		
		super
	end
end

class WaitTimeout < StandardError
end

# Exercise both exceptional exits from block and synchronous operations during cancellation cleanup.
class InterruptedBlockScheduler < IO::Event::TestScheduler
	attr_reader :nested_result, :blocking_states
	
	def initialize(...)
		super
		@blocking_states = []
	end
	
	def block(blocker, timeout = nil)
		return super unless blocker.is_a?(IO::Event::WorkerPool)
		
		@blocking_states << Fiber.current.blocking?
		case @blocking_states.size
		when 1
			raise WaitTimeout, "original interruption"
		when 2
			@nested_result = IO::Event::WorkerPool.busy(duration: 0.01)
			raise WaitTimeout, "cleanup interruption"
		end
		
		super
	end
end

describe IO::Event::WorkerPool do
	with "an instance" do
		let(:worker_pool) {subject.new}
		
		after do
			worker_pool&.close
		end
		
		it "can create a worker pool" do
			expect(worker_pool).to be_a(IO::Event::WorkerPool)
		end
		
		it "provides statistics" do
			# Force initialization by calling a method on the pool
			pool = worker_pool # This should trigger initialization
			statistics = pool.statistics
			
			expect(statistics).to be_a(Hash)
			expect(statistics).to have_keys(
				current_worker_count: be_a(Integer),
				maximum_worker_count: be == 1,
				current_queue_size: be == 0,
				shutdown: be == false
			)
		end
		
		it "can close the worker pool" do
			pool = worker_pool
			
			# Check that it's not shut down initially
			expect(pool.statistics[:shutdown]).to be == false
			
			# Close the pool
			result = pool.close
			expect(result).to be_nil
			
			# Check that it's now shut down
			expect(pool.statistics[:shutdown]).to be == true
			expect(pool.statistics[:current_worker_count]).to be == 0
		end
		
		it "can close the worker pool multiple times safely" do
			pool = worker_pool
			
			# Close the pool twice
			pool.close
			pool.close
			
			# Should still be shut down
			expect(pool.statistics[:shutdown]).to be == true
		end
	end
	
	with IO::Event::TestScheduler do
		let(:scheduler) {IO::Event::TestScheduler.new}
		
		it "can create a test scheduler" do
			expect(scheduler).to be_a(IO::Event::TestScheduler)
			expect(scheduler.worker_pool).to be_a(IO::Event::WorkerPool)
		end
		
		it "interrupts IO::Buffer.copy operations larger than 1MiB" do
			skip "IO::Buffer not available" unless defined?(IO::Buffer)
			
			# Create buffers larger than 1MiB to trigger GVL release
			buffer_size = 2 * 1024 * 1024  # 2MiB
			source = IO::Buffer.new(buffer_size)
			destination = IO::Buffer.new(buffer_size)
			
			# Fill source buffer with some data
			source.clear("A".ord)
			worker_pool = nil
			
			Thread.new do
				Fiber.set_scheduler(scheduler)
				worker_pool = scheduler.worker_pool
				
				# Perform the large copy operation in a scheduled fiber
				Fiber.schedule do
					destination.copy(source, 0, buffer_size, 0)
				end
			end.join
			
			# Confirm that the copy worked:
			expect(destination.get_string(0, 10)).to be == "AAAAAAAAAA"
			
			expect(worker_pool.statistics[:call_count]).to be > 0
			expect(worker_pool.statistics[:completed_count]).to be > 0
			inform worker_pool.statistics
		end
	end
	
	with "cancellable busy operation" do
		let(:scheduler) {IO::Event::TestScheduler.new}
		
		it "can perform a busy operation that completes normally" do
			start_time = Time.now
			result = IO::Event::WorkerPool.busy(duration: 0.1)
			end_time = Time.now
			elapsed = end_time - start_time
			
			expect(result).to be_a(Hash)
			expect(result[:cancelled]).to be == false
			expect(result[:result]).to be == :completed
		end
		
		it "can perform a busy operation with different durations" do
			result = IO::Event::WorkerPool.busy(duration: 0.05)
			
			expect(result).to be_a(Hash)
			expect(result[:cancelled]).to be == false
			expect(result[:result]).to be == :completed
			expect(result[:duration]).to be == 0.05
		end
		
		it "keeps blocked fibers alive during garbage collection" do
			result = nil
			
			Thread.new do
				Fiber.set_scheduler(scheduler)
				
				gc_thread = Thread.new do
					sleep(0.02)
					GC.start(full_mark: true, immediate_sweep: true)
				end
				
				Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 0.1)
				end
				
				scheduler.run
				gc_thread.value
			ensure
				Fiber.set_scheduler(nil)
				scheduler.close
			end.value
			
			expect(result).to have_keys(
				cancelled: be == false,
				result: be == :completed
			)
		end
		
		it "can cancel a busy operation using unblock function" do
			# This tests the cancellation mechanism through rb_thread_call_without_gvl
			completed = false
			thread = Thread.new do
				start_time = Time.now
				result = IO::Event::WorkerPool.busy(duration: 1.0)  # Long operation
				end_time = Time.now
				elapsed = end_time - start_time
				completed = true
				
				{result: result, elapsed: elapsed}
			end
			
			# Let it start, then kill the thread (which should trigger the unblock function)
			sleep(0.1)
			thread.kill
			thread.join(0.5)  # Wait up to 0.5s for thread to finish
			
			# The operation should have been interrupted before completion
			expect(completed).to be == false
		end
		
		it "can be cancelled when executed in a worker pool" do
			result = nil
			elapsed = nil
			error = nil
			
			Thread.new do
				Fiber.set_scheduler(scheduler)
				
				busy_fiber = Fiber.schedule do
					start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
					result = IO::Event::WorkerPool.busy(duration: 2.0)
				rescue Interrupt => error
					# Ignore.
				ensure
					end_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
					elapsed = end_time - start_time
				end
				
				Fiber.schedule do
					sleep(0.5)
					Fiber.scheduler.fiber_interrupt(busy_fiber, StandardError)
				end
			end.join
			
			expect(result[:cancelled]).to be == true
			expect(elapsed).to be < 1.0
			expect(error).to be_nil
		end
		
		it "can be cancelled before even starting" do
			result = nil
			
			Thread.new do
				Fiber.set_scheduler(scheduler)
				
				busy_fiber = Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 2.0)
				end
				
				Fiber.schedule do
					Fiber.scheduler.fiber_interrupt(busy_fiber, StandardError)
				end
			end.join
			
			expect(result).to have_keys(
				cancelled: be == true,
				result: be == :exception,
				exception: be_a(StandardError)
			)
		end
	end
	
	with "scheduler that rescues internally in block" do
		# A scheduler whose block method raises on the first call
		# and rescues on a subsequent call clearing ec->errinfo.
		class RescuingBlockScheduler < IO::Event::TestScheduler
			attr_reader :rescued
			
			def initialize(...)
				super
				@block_call_count = 0
			end
			
			def block(blocker, timeout = nil)
				@block_call_count += 1
				
				raise StandardError, "first block failure" if @block_call_count == 1
				
				begin
					raise StandardError, "internal block error"
				rescue
					@rescued = true
				end
				super
			end
		end
		
		it "preserves the original exception through block rescue" do
			result = nil
			rescued = false
			
			Thread.new do
				scheduler = RescuingBlockScheduler.new
				Fiber.set_scheduler(scheduler)
				
				Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 2.0)
				end
				rescued = scheduler.rescued
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end.value
			
			expect(result).to be_a(Hash)
			expect(result[:result]).to be == :exception
			expect(result[:exception]).to be_a(StandardError)
			expect(result[:exception].message).to be == "first block failure"
			expect(rescued).to be == true
		end
	end
	
	with "scheduler that throws from block" do
		class ThrowingBlockScheduler < IO::Event::TestScheduler
			attr_reader :rescued
			
			def initialize(...)
				super
				@block_call_count = 0
			end
			
			def block(...)
				@block_call_count += 1
				
				throw :worker_pool_escape, :expected if @block_call_count == 1
				
				begin
					raise StandardError, "internal block error"
				rescue
					@rescued = true
				end
				
				super
			end
		end
		
		it "preserves non-exception control flow" do
			result = nil
			rescued = false
			blocking = nil
			
			Thread.new do
				scheduler = ThrowingBlockScheduler.new
				Fiber.set_scheduler(scheduler)
				
				Fiber.schedule do
					result = catch(:worker_pool_escape) do
						IO::Event::WorkerPool.busy(duration: 2.0)
						:missing
					end
					blocking = Fiber.current.blocking?
				end
				rescued = scheduler.rescued
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end.value
			
			expect(result).to be == :expected
			expect(rescued).to be == true
			expect(blocking).to be == false
		end
	end
	
	with "blocking operations inside the scheduler block hook" do
		it "wakes the outer operation and runs the nested operation" do
			result = nil
			nested_result = nil
			statistics = nil
			blocking_states = nil
			blocking = nil
			
			Thread.new do
				# The nested operation must finish synchronously without using the single worker:
				scheduler = ReentrantBlockScheduler.new(maximum_worker_count: 1)
				Fiber.set_scheduler(scheduler)
				
				fiber = Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 0.1)
					blocking = Fiber.current.blocking?
				end
				
				# Bound the outer wait so a lost wakeup fails the test instead of hanging it:
				Fiber.schedule do
					deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 2.0
					
					while result.nil? and Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
						sleep(0.01)
					end
					
					Fiber.scheduler.fiber_interrupt(fiber, WaitTimeout) if result.nil?
				end
				
				scheduler.run
				nested_result = scheduler.nested_result
				statistics = scheduler.worker_pool.statistics
				blocking_states = scheduler.blocking_states
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end.join
			
			# The nested operation should run to completion, rather than being cancelled when the outer operation's wakeup resumes the nested wait (depending on timing, the cancelled operation is either interrupted or skipped entirely):
			expect(nested_result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
			
			# The outer operation should complete without being interrupted by the timeout:
			expect(result[:exception]).to be_nil
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
			expect(statistics).to have_keys(call_count: be == 1, completed_count: be == 1, current_queue_size: be == 0)
			expect(blocking_states).to be == [true]
			expect(blocking).to be == false
		end
	end
	
	with "a blocking wait" do
		it "allows other fibers to offload operations while waiting" do
			results = []
			blocking_states = []
			statistics = nil
			
			Thread.new do
				scheduler = IO::Event::TestScheduler.new(maximum_worker_count: 1)
				Fiber.set_scheduler(scheduler)
				
				2.times do
					Fiber.schedule do
						blocking_states << Fiber.current.blocking?
						results << IO::Event::WorkerPool.busy(duration: 0.01)
						blocking_states << Fiber.current.blocking?
					end
				end
				
				# Both fibers have submitted their operations and suspended:
				statistics = scheduler.worker_pool.statistics
				scheduler.run
			ensure
				Fiber.set_scheduler(nil)
			end.value
			
			expect(statistics[:call_count]).to be == 2
			expect(results.size).to be == 2
			results.each do |result|
				expect(result).to have_keys(executed: be == true, result: be == :completed)
			end
			expect(blocking_states).to be == [false, false, false, false]
		end
		
		it "keeps cancellation cleanup blocking and restores the fiber after an exception" do
			result = nil
			next_result = nil
			blocking = nil
			statistics = nil
			scheduler = nil
			
			Thread.new do
				scheduler = InterruptedBlockScheduler.new
				Fiber.set_scheduler(scheduler)
				
				Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 2.0)
					blocking = Fiber.current.blocking?
					next_result = IO::Event::WorkerPool.busy(duration: 0.01)
				end
				
				scheduler.run
				statistics = scheduler.worker_pool.statistics
			ensure
				Fiber.set_scheduler(nil)
			end.value
			
			expect(result).to have_keys(result: be == :exception, exception: be_a(WaitTimeout).and(have_attributes(message: be == "original interruption")))
			expect(scheduler.nested_result).to have_keys(executed: be == true, result: be == :completed)
			expect(next_result).to have_keys(executed: be == true, result: be == :completed)
			expect(statistics).to have_keys(call_count: be == 2, completed_count: be == 2, current_queue_size: be == 0)
			expect(scheduler.blocking_states).not.to be(:include?, false)
			expect(blocking).to be == false
		end
		
		it "does not cancel work on a spurious wakeup" do
			result = nil
			
			Thread.new do
				scheduler = IO::Event::TestScheduler.new
				Fiber.set_scheduler(scheduler)
				
				fiber = Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 0.1)
				end
				
				scheduler.unblock(nil, fiber)
				scheduler.run
			ensure
				Fiber.set_scheduler(nil)
			end.value
			
			expect(result).to have_keys(executed: be == true, result: be == :completed)
		end
	end
end
