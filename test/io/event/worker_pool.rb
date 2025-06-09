# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "io/event"
require "io/event/test_scheduler"

return unless defined?(IO::Event::WorkerPool)

describe IO::Event::WorkerPool do		
	with "an instance" do
		let(:worker_pool) { subject.new(max_threads: 2) }
		
		after do
			worker_pool = nil # This should trigger GC cleanup
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
				maximum_worker_count: be == 2,
				current_queue_size: be == 0,
				shutdown: be == false
			)
		end
	end
	
	with "TestScheduler integration" do
		let(:scheduler) {IO::Event::TestScheduler.new(max_threads: 1)}
		
		it "can create a test scheduler" do
			expect(scheduler).to be_a(IO::Event::TestScheduler)
			expect(scheduler.worker_pool).to be_a(IO::Event::WorkerPool)
		end
		
		it "intercepts IO::Buffer.copy operations larger than 1MiB" do
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
end
