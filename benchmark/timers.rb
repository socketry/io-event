# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "sus/fixtures/benchmark"
require "io/event/timers"

# Measures timer scheduling and cancellation behavior, including the cost of
# retaining cancelled timers in the priority heap.
#
# Run with: bundle exec sus --verbose benchmark/timers.rb

describe IO::Event::Timers do
	include Sus::Fixtures::Benchmark
	
	COUNT = 10_000
	
	def schedule_and_flush(timers, count, base_time: timers.now, offset: 0)
		count.times do |index|
			timers.schedule(base_time + offset + index, proc{})
		end
		
		timers.size
	end
	
	def schedule_flush_and_cancel(timers, count, base_time: timers.now, offset: 0)
		handles = []
		
		count.times do |index|
			handles << timers.schedule(base_time + offset + index, proc{})
		end
		
		timers.size
		handles.each(&:cancel!)
	end
	
	measure "schedule and flush #{COUNT} timers" do |repeats|
		repeats.exactly(16).times do
			timers = subject.new
			
			schedule_and_flush(timers, COUNT)
		end
	end
	
	measure "cancel #{COUNT} timers before flush" do |repeats|
		repeats.exactly(16).times do
			timers = subject.new
			handles = []
			base_time = timers.now
			
			COUNT.times do |index|
				handles << timers.schedule(base_time + index, proc{})
			end
			
			handles.each(&:cancel!)
			timers.size
		end
	end
	
	measure "cancel #{COUNT} timers after flush" do |repeats|
		repeats.exactly(16).times do
			timers = subject.new
			
			schedule_flush_and_cancel(timers, COUNT)
		end
	end
	
	measure "schedule and flush #{COUNT} timers after #{COUNT} cancelled heap timers" do |repeats|
		repeats.exactly(16).times do
			timers = subject.new
			base_time = timers.now
			
			schedule_flush_and_cancel(timers, COUNT, base_time: base_time, offset: COUNT)
			schedule_and_flush(timers, COUNT, base_time: base_time)
		end
	end
	
	measure "wait interval with #{COUNT} cancelled heap timers" do |repeats|
		repeats.exactly(16).times do
			timers = subject.new
			base_time = timers.now
			
			schedule_flush_and_cancel(timers, COUNT, base_time: base_time)
			timers.wait_interval(base_time)
		end
	end
end
