# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2023-2024, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

Interruptable = Sus::Shared("interruptable") do
	it "can interrupt sleeping selector" do
		result = nil
		
		thread = Thread.new do
			Thread.current.report_on_exception = false
			selector = subject.new(Fiber.current)
			
			Thread.handle_interrupt(::SignalException => :never) do
				result = selector.select(nil)
			end
		end
		
		# Wait for thread to enter the selector:
		sleep(0.001) until thread.status == "sleep"
		
		thread.raise(::Interrupt)
		
		expect{thread.join}.to raise_exception(::Interrupt)
		expect(result).to be == 0
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		it_behaves_like Interruptable
	end
end
