# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "sus/fixtures/benchmark"
require "io/event"

# Measures the cross-thread wakeup roundtrip for each selector backend:
# the time from when an external thread calls selector.wakeup to when the
# blocking select returns in the owner thread.
#
# Run with: bundle exec sus --verbose benchmark/selector_wakeup.rb

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	next unless klass.respond_to?(:new)
	
	describe "#{klass}" do
		include Sus::Fixtures::Benchmark
		
		# Roundtrip: another thread signals wakeup, we measure how long select
		# takes to unblock. A Queue ensures the wakeup is always fired *after*
		# select has started blocking.
		measure "cross-thread wakeup roundtrip" do |repeats|
			selector = klass.new(Fiber.current)
			
			# Waker thread: each iteration it receives a token then fires wakeup.
			go = Queue.new
			waker = Thread.new do
				loop do
					go.pop
					selector.wakeup
				end
			end
			
			repeats.times do
				# Signal the waker then immediately block — wakeup arrives shortly.
				go.push(nil)
				selector.select(1)
			end
			
			waker.kill
			waker.join rescue nil
			selector.close
		end
		
		# Baseline: cost of calling wakeup when the selector is not blocking.
		measure "wakeup while idle" do |repeats|
			selector = klass.new(Fiber.current)
			
			repeats.times do
				selector.wakeup
			end
			
			selector.close
		end
	end
end
