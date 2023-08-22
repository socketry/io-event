# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require 'io/event'
require 'io/event/selector'
require 'socket'

Interrupt = Sus::Shared("interrupt") do
	let(:pipe) {IO.pipe}
	let(:input) {pipe.first}
	let(:output) {pipe.last}
	
	it "can interrupt sleeping selector" do
		thread = Thread.new do
			Fiber.new do
				selector.io_wait(Fiber.current, input, IO::READABLE)
			end
			
			Thread.handle_interrupt(::SignalException => :never) do
				selector.select(nil)
			end
		end
		
		sleep(1.0)
		
		thread.raise(::Interrupt)
		
		expect{thread.join}.to be_a(::Interrupt)
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		def before
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		def after
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like Interrupt
	end
end
