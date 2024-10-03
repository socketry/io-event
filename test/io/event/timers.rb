# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2024, by Samuel Williams.

require "io/event/timers"

class FloatWrapper
	def initialize(value)
		@value = value
	end
	
	def to_f
		@value
	end
end

describe IO::Event::Timers do
	let(:timers) {subject.new}
	
	it "should register an event" do
		fired = false
		
		callback = proc do |_time|
			fired = true
		end
		
		timers.after(0.1, &callback)
		
		expect(timers.size).to be == 1
		
		timers.fire(timers.now + 0.15)
		
		expect(timers.size).to be == 0
		
		expect(fired).to be == true
	end
	
	it "should register timers in order" do
		fired = []
		
		offsets = [0.95, 0.1, 0.3, 0.5, 0.4, 0.2, 0.01, 0.9]
		
		offsets.each do |offset|
			timers.after(offset) do
				fired << offset
			end
		end
		
		timers.fire(timers.now + 0.5)
		expect(fired).to be == offsets.sort.first(6)
		
		timers.fire(timers.now + 1.0)
		expect(fired).to be == offsets.sort
	end
	
	it "should fire timers with the time they were fired at" do
		fired_at = :not_fired
		
		timers.after(0.5) do |time|
			# The time we actually were fired at:
			fired_at = time
		end
		
		now = timers.now + 1.0
		timers.fire(now)
		
		expect(fired_at).to be == now
	end
	
	it "should flush cancelled timers" do
		10.times do
			handle = timers.after(0.1) {}
			handle.cancel!
		end
		
		expect(timers.size).to be == 0
	end
	
	with "#schedule" do
		it "raises an error if given an invalid time" do
			expect do
				timers.after(Object.new) {}
			end.to raise_exception(NoMethodError, message: be =~ /to_f/)
		end
		
		it "converts the offset to a float" do
			fired = false
			
			timers.after(FloatWrapper.new(0.1)) do
				fired = true
			end
			
			timers.fire(timers.now + 0.15)
			
			expect(fired).to be == true
		end
	end
	
	with "#wait_interval" do
		it "should return nil if no timers are scheduled" do
			expect(timers.wait_interval).to be_nil
		end
		
		it "should return nil if all timers are cancelled" do
			handle = timers.after(0.1) {}
			handle.cancel!
			
			expect(timers.wait_interval).to be_nil
		end
		
		it "should return the time until the next timer" do
			timers.after(0.1) {}
			timers.after(0.2) {}
			
			expect(timers.wait_interval).to be_within(0.01).of(0.1)
		end
	end
end
