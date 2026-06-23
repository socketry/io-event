# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/interrupt"

describe IO::Event.const_get(:Interrupt) do
	let(:loop) {Fiber.current}
	
	let(:selector) do
		loop = self.loop
		
		Object.new.tap do |selector|
			selector.define_singleton_method(:io_wait) do |fiber, io, events|
				loop.transfer
				
				return true
			end
		end
	end
	
	with "#close" do
		it "handles IOError in the interrupt fiber" do
			interrupt = subject.new(selector)
			fiber = interrupt.instance_variable_get(:@fiber)
			
			interrupt.close
			
			expect do
				fiber.transfer
			end.not.to raise_exception(IOError)
			
			expect(fiber).not.to be(:alive?)
		end
	end
end
