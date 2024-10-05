# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2024, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "tempfile"

FileIO = Sus::Shared("file io") do
	with "a file" do
		let(:file) {Tempfile.new}
		
		it "can read using a buffer" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			write_result = nil
			read_result = nil
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				file.seek(0)
				write_result = selector.io_write(Fiber.current, file, buffer, 128)
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				file.seek(0)
				read_result = selector.io_read(Fiber.current, file, buffer, 0)
			end
			
			# The seek and the read/write are potentially racing since we use the same file descriptor. So we wait for the write to complete, and then wait for the read to complete.
			
			writer.transfer
			
			while write_result.nil?
				selector.select(0)
			end
			
			reader.transfer
			
			while read_result.nil?
				selector.select(0)
			end
			
			expect(write_result).to be == 128
			expect(read_result).to be == 64
		end
		
		it "can wait for the file to become writable" do
			wait_result = nil
			
			writer = Fiber.new do
				wait_result = selector.io_wait(Fiber.current, file, IO::WRITABLE)
			end
			
			writer.transfer
			
			selector.select(0)
			
			expect(wait_result).to be == IO::WRITABLE
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	# Don't run the test if the selector doesn't support `io_read`/`io_write`:
	next unless klass.instance_methods.include?(:io_read)
	
	describe(klass, unique: name) do
		before do
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		after do
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like FileIO
	end
end
