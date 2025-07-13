# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2025, by Samuel Williams.
# Copyright, 2023, by Math Ieu.

require "io/event"
require "io/event/selector"
require "socket"

require "unix_socket"

BufferedIO = Sus::Shared("buffered io") do
	with "a pipe" do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		it "can read using a buffer" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
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
		
		it "can write zero length buffers" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			buffer = IO::Buffer.new(1).slice(0, 0)
			expect(selector.io_write(Fiber.current, output, buffer, 0)).to be == 0
		end
		
		it "can read and write at the specified offset" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				# We can't write 128 bytes because there are only +64 bytes from offset 64.
				expect(selector.io_write(Fiber.current, output, buffer, 128, 64)).to be == 64
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(128)
				# Only 64 bytes are available to read.
				expect(selector.io_read(Fiber.current, input, buffer, 1, 64)).to be == 64
			end
			
			reader.transfer
			writer.transfer
			
			expect(selector.select(1)).to be >= 1
		end
		
		it "can't write to the read end of a pipe" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			output.close
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(64)
				result = selector.io_write(Fiber.current, input, buffer, 64)
				expect(result).to be < 0
			end
			
			writer.transfer
			selector.select(0)
		end
		
		it "can perform non-blocking read" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			buffer = IO::Buffer.new(64)
			result = nil
			
			output.puts "Hello World\n"
			output.close
			
			reader = Fiber.new do
				result = selector.io_read(Fiber.current, input, buffer, 0)
			end
			
			reader.transfer
			selector.select(0)
			
			expect(buffer.get_string(0, 12)).to be == "Hello World\n"
		end
		
		# Whether the given error code indicates that the operation should be retried.
		def be_again?
			(be == -Errno::EAGAIN::Errno).or(be == -Errno::EWOULDBLOCK::Errno)
		end
		
		it "can perform non-blocking read with empty input" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			buffer = IO::Buffer.new(64)
			result = nil
			
			reader = Fiber.new do
				result = selector.io_read(Fiber.current, input, buffer, 0)
			end
			
			reader.transfer
			selector.select(0)
			
			expect(result).to be_again?
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
		
		it_behaves_like BufferedIO
	end
end
