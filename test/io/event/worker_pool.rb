# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025-2026, by Samuel Williams.
# Copyright, 2026, by Fletcher Dares.

require "io/event"
require "io/event/test_scheduler"

return unless defined?(IO::Event::WorkerPool)

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
			
			Thread.new do
				scheduler = ThrowingBlockScheduler.new
				Fiber.set_scheduler(scheduler)
				
				Fiber.schedule do
					result = catch(:worker_pool_escape) do
						IO::Event::WorkerPool.busy(duration: 2.0)
						:missing
					end
				end
				rescued = scheduler.rescued
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end.value
			
			expect(result).to be == :expected
			expect(rescued).to be == true
		end
	end
	
	with "scheduler that re-enters the worker pool from block" do
		# Ruby can run GC finalizers inside the scheduler's `block` hook, before the fiber suspends. If a finalizer performs an offloaded blocking operation (e.g. `File.unlink` from a `Tempfile` finalizer), the same fiber calls `WorkerPool#call` again while its outer wait is still in progress. This scheduler simulates that deterministically by performing nested blocking operations from inside the worker pool `block`, one per duration in `nested`, each from inside the previous one.
		class ReentrantBlockScheduler < IO::Event::TestScheduler
			def initialize(nested: [0.01], before_nested: nil, **options)
				super(**options)
				
				@nested = nested.dup
				@before_nested = before_nested
				@nested_results = []
			end
			
			# @attribute [Array(Hash)] The results of the nested operations, innermost first.
			attr_reader :nested_results
			
			def nested_result
				@nested_results.first
			end
			
			def block(blocker, timeout = nil)
				# Simulate a finalizer that performs an offloaded blocking operation:
				if blocker.is_a?(IO::Event::WorkerPool) and duration = @nested.shift
					@before_nested&.call(self)
					
					result = IO::Event::WorkerPool.busy(duration: duration)
					@nested_results << result
					
					# `busy` rescues the exception that cancelled it, so propagate it to the outer wait:
					raise result[:exception] if result[:exception]
				end
				
				super
			end
		end
		
		class WaitTimeout < StandardError
		end
		
		class Stop < StandardError
		end
		
		# Run `busy(duration:)` in a fiber on a new `ReentrantBlockScheduler`, interrupting it with `WaitTimeout` if it hasn't finished within `timeout` seconds, so a lost wakeup fails the test instead of hanging it.
		#
		# @returns [Array(Hash, ReentrantBlockScheduler)] The result of the outer operation, and the scheduler.
		def run_busy(duration:, timeout: 2.0, **options, &block)
			result = nil
			scheduler = nil
			
			Thread.new do
				scheduler = ReentrantBlockScheduler.new(**options)
				Fiber.set_scheduler(scheduler)
				
				fiber = Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: duration)
					result[:statistics] = scheduler.worker_pool.statistics
				end
				
				Fiber.schedule do
					deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + timeout
					
					while result.nil? and Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
						sleep(0.01)
					end
					
					Fiber.scheduler.fiber_interrupt(fiber, WaitTimeout) if result.nil?
				end
				
				block&.call(fiber)
				
				scheduler.run
			ensure
				Fiber.set_scheduler(nil)
				scheduler&.close
			end.join
			
			return result, scheduler
		end
		
		it "wakes the outer operation and runs the nested operation" do
			# A single worker guarantees that the nested operation queues behind the outer one:
			result, scheduler = run_busy(duration: 0.1, maximum_worker_count: 1)
			
			# The nested operation should run to completion, rather than being cancelled when the outer operation's wakeup resumes the nested wait (depending on timing, the cancelled operation is either interrupted or skipped entirely):
			expect(scheduler.nested_result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
			
			# The outer operation should complete without being interrupted by the timeout:
			expect(result[:exception]).to be_nil
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
		
		it "wakes the outer operation when it completed before the nested operation started" do
			completed_count = nil
			
			before_nested = proc do |scheduler|
				# Give the worker time to complete the outer operation and unblock the fiber:
				deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 0.1
				Thread.pass while Process.clock_gettime(Process::CLOCK_MONOTONIC) < deadline
				
				completed_count = scheduler.worker_pool.statistics[:completed_count]
			end
			
			result, scheduler = run_busy(duration: 0.0, maximum_worker_count: 1, before_nested: before_nested)
			
			expect(completed_count).to be == 1
			expect(scheduler.nested_result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
		
		it "wakes the outer operation when the nested operation finishes first" do
			# With two workers, the nested operation runs concurrently and finishes long before the outer one:
			result, scheduler = run_busy(duration: 0.2, maximum_worker_count: 2, nested: [0.01])
			
			expect(scheduler.nested_result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
		
		it "wakes every operation with two levels of nesting" do
			result, scheduler = run_busy(duration: 0.05, maximum_worker_count: 1, nested: [0.02, 0.01])
			
			expect(scheduler.nested_results.size).to be == 2
			scheduler.nested_results.each do |nested_result|
				expect(nested_result).to have_keys(
					executed: be == true,
					result: be == :completed
				)
			end
			
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
		
		it "wakes every operation with two levels of nesting and multiple workers" do
			result, scheduler = run_busy(duration: 0.05, maximum_worker_count: 3, nested: [0.1, 0.01])
			
			expect(scheduler.nested_results.size).to be == 2
			scheduler.nested_results.each do |nested_result|
				expect(nested_result).to have_keys(
					executed: be == true,
					result: be == :completed
				)
			end
			
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
		
		it "cancels and drains both operations when interrupted while nested" do
			start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
			
			# With two workers, both operations are in flight when the exception is raised:
			result, scheduler = run_busy(duration: 2.0, maximum_worker_count: 2, nested: [2.0]) do |fiber|
				Fiber.schedule do
					sleep(0.1)
					Fiber.scheduler.fiber_interrupt(fiber, Stop)
				end
			end
			
			elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start_time
			
			# The exception cancels the nested operation and then, propagating from `block`, the outer operation:
			expect(scheduler.nested_result).to have_keys(
				result: be == :exception,
				exception: be_a(Stop)
			)
			expect(result).to have_keys(
				result: be == :exception,
				exception: be_a(Stop)
			)
			expect(elapsed).to be < 1.0
			
			# Both operations were finished by the workers before returning:
			expect(result[:statistics]).to have_keys(
				call_count: be == 2,
				completed_count: be == 2,
				current_queue_size: be == 0
			)
		end
	end
	
	with "spurious wakeup" do
		let(:scheduler) {IO::Event::TestScheduler.new}
		
		it "does not cancel the operation" do
			result = nil
			
			Thread.new do
				Fiber.set_scheduler(scheduler)
				
				fiber = Fiber.schedule do
					result = IO::Event::WorkerPool.busy(duration: 0.1)
				end
				
				# Resume the waiting fiber without an exception, e.g. a stale `unblock`:
				Fiber.schedule do
					Fiber.scheduler.unblock(nil, fiber)
				end
				
				scheduler.run
			ensure
				Fiber.set_scheduler(nil)
				scheduler.close
			end.join
			
			expect(result).to have_keys(
				executed: be == true,
				result: be == :completed
			)
		end
	end
end
