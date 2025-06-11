# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

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
	
	with "TestScheduler integration" do
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
	end
end
