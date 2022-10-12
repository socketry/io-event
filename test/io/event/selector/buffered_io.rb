# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'io/event'
require 'io/event/selector'
require 'socket'

return unless IO::Event::Support.buffer?

BufferedIO = Sus::Shared("buffered io") do
	with 'a pipe' do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		it "can read using a buffer" do
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				expect(selector.io_write(Fiber.current, output, buffer, 128)).to be == 128
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				expect(selector.io_read(Fiber.current, input, buffer, 1)).to be == 64
			end
			
			reader.transfer
			writer.transfer
			
			expect(selector.select(1)).to be >= 1
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	next unless klass.instance_methods.include?(:io_read)
	
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
		
		it_behaves_like BufferedIO
	end
end
