# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.
# Copyright, 2023, by Math Ieu.

require 'io/event'
require 'io/event/selector'
require 'socket'

require 'unix_socket'

BufferedIO = Sus::Shared("buffered io") do
	with 'a pipe' do
		let(:pipe) {IO.pipe}
		let(:input) {pipe.first}
		let(:output) {pipe.last}
		
		it "can read using a buffer" do
			# Non-blocking pipes are probably not implemented in Ruby's compatibility layer.
			# https://learn.microsoft.com/en-gb/windows/win32/api/namedpipeapi/nf-namedpipeapi-setnamedpipehandlestate?redirectedfrom=MSDN
			skip "Not supported on Windows" if RUBY_PLATFORM =~ /mswin|mingw|cygwin/
			
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
			buffer = IO::Buffer.new(1).slice(0, 0)
			expect(selector.io_write(Fiber.current, output, buffer, 0)).to be == 0
		end
		
		it "can read and write at the specified offset" do
			skip "Not supported on Windows" if RUBY_PLATFORM =~ /mswin|mingw|cygwin/
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				expect(selector.io_write(Fiber.current, output, buffer, 128, 64)).to be == 64
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(128)
				expect(selector.io_read(Fiber.current, input, buffer, 1, 64)).to be == 64
			end
			
			reader.transfer
			writer.transfer
			
			expect(selector.select(1)).to be >= 1
		end
		
		it "can't write to the read end of a pipe" do
			skip "Windows is bonkers" if RUBY_PLATFORM =~ /mswin|mingw|cygwin/
			bsd = RUBY_PLATFORM =~ /bsd/
			
			output.close if bsd # BSD traditionally has bidirectional pipes
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(64)
				result = selector.io_write(Fiber.current, input, buffer, 64)
				expect(result).to be < 0
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
		
		it_behaves_like BufferedIO
	end
end
