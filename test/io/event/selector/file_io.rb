# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require 'io/event'
require 'io/event/selector'
require 'tempfile'

FileIO = Sus::Shared("file io") do
	with 'a file' do
		let(:file) {Tempfile.new}
		
		it "can read using a buffer" do
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				file.seek(0)
				expect(selector.io_write(Fiber.current, file, buffer, 128)).to be == 128
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				file.seek(0)
				expect(selector.io_read(Fiber.current, file, buffer, 1)).to be == 64
			end
			
			writer.transfer
			reader.transfer
		end
		
		it "can wait for the file to become writable" do
			writer = Fiber.new do
				expect(
					selector.io_wait(Fiber.current, file, IO::WRITABLE)
				).to be == IO::WRITABLE
			end
			
			writer.transfer
			
			selector.select(0)
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	# Don't run the test if the selector doesn't support `io_read`/`io_write`:
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
		
		it_behaves_like FileIO
	end
end
