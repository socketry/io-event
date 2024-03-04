# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024, by Samuel Williams.

require 'io/event/timers'

describe IO::Event::Timers do
	let(:timers) {subject.new}
	
	it "should register an event" do
		fired = false
		
		callback = proc do |_time|
			fired = true
		end
		
		timers.schedule(0.1, callback)
		
		expect(timers.size).to be == 1
		
		timers.fire(0.15)
		
		expect(timers.size).to be == 0
		
		expect(fired).to be == true
	end
	
	it "should register timers in order" do
		fired = []
		
		times = [0.95, 0.1, 0.3, 0.5, 0.4, 0.2, 0.01, 0.9]
		
		times.each do |requested_time|
			callback = proc do |_time|
				fired << requested_time
			end
			
			timers.schedule(requested_time, callback)
		end
		
		timers.fire(0.5)
		expect(fired).to be == times.sort.first(6)
		
		timers.fire(1.0)
		expect(fired).to be == times.sort
	end
	
	it "should fire timers with the time they were fired at" do
		fired_at = :not_fired
		
		callback = proc do |time|
			# The time we actually were fired at:
			fired_at = time
		end
		
		timers.schedule(0.5, callback)
		
		timers.fire(1.0)
		
		expect(fired_at).to be == 1.0
	end
	
	it "should flush cancelled timers" do
		callback = proc{}
		
		10.times do
			handle = timers.schedule(0.1, callback)
			handle.cancel!
		end
		
		expect(timers.size).to be == 0
	end
end
