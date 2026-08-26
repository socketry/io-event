# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "tempfile"

FileIO = Sus::Shared("file io") do
	def io_read(io, buffer, offset, length)
		selector.io_read(Fiber.current, io, buffer, offset, length)
	end
	
	def io_write(io, buffer, offset, length)
		selector.io_write(Fiber.current, io, buffer, offset, length)
	end
	
	def io_pread(io, buffer, from, offset, length)
		selector.io_pread(Fiber.current, io, buffer, from, offset, length)
	end
	
	def io_pwrite(io, buffer, from, offset, length)
		selector.io_pwrite(Fiber.current, io, buffer, from, offset, length)
	end
	
	def await_io
		result = nil
		fiber = Fiber.new do
			result = yield
		end
		
		fiber.transfer
		selector.select(1) while result.nil? && fiber.alive?
		
		return result
	end
	
	with "a file" do
		let(:file) {Tempfile.new}
		
		it "can read using a buffer" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			write_result = nil
			read_result = nil
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				file.seek(0)
				write_result = io_write(file, buffer, 0, 128)
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				file.seek(0)
				
				read_result = io_read(file, buffer, 0, 64)
			end
			
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
		
		it "can pread using a buffer" do
			skip "io_pread is not implemented" unless selector.respond_to?(:io_pread)
			
			write_result = nil
			read_result = nil
			
			writer = Fiber.new do
				buffer = IO::Buffer.new(128)
				write_result = io_pwrite(file, buffer, 0, 0, 128)
			end
			
			reader = Fiber.new do
				buffer = IO::Buffer.new(64)
				read_result = io_pread(file, buffer, 0, 0, 64)
			end
			
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
		
		it "uses positional IO ranges" do
			skip "io_pread is not implemented" unless selector.respond_to?(:io_pread)
			
			write_buffer = IO::Buffer.for("01234567".dup)
			expect(await_io{io_pwrite(file, write_buffer, 5, 2, 3)}).to be == 3
			
			read_buffer = IO::Buffer.new(8)
			expect(await_io{io_pread(file, read_buffer, 5, 1, 3)}).to be == 3
			expect(read_buffer.get_string(1, 3)).to be == "234"
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
		
		it "returns EINVAL when read offset exceeds buffer size" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			buffer = IO::Buffer.new(64)
			file.seek(0)
			
			# Offset 128 exceeds buffer size of 64
			result = io_read(file, buffer, 128, 1)
			
			expect(result).to be == -Errno::EINVAL::Errno
		end
		
		it "returns EINVAL when write offset exceeds buffer size" do
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			buffer = IO::Buffer.new(64)
			file.seek(0)
			
			# Offset 128 exceeds buffer size of 64
			result = io_write(file, buffer, 128, 1)
			
			expect(result).to be == -Errno::EINVAL::Errno
		end
		
		it "returns EINVAL when pread offset exceeds buffer size" do
			skip "io_pread is not implemented" unless selector.respond_to?(:io_pread)
			
			buffer = IO::Buffer.new(64)
			
			# Offset 128 exceeds buffer size of 64
			result = io_pread(file, buffer, 0, 128, 1)
			
			expect(result).to be == -Errno::EINVAL::Errno
		end
		
		it "returns EINVAL when pwrite offset exceeds buffer size" do
			skip "io_pwrite is not implemented" unless selector.respond_to?(:io_pwrite)
			
			buffer = IO::Buffer.new(64)
			
			# Offset 128 exceeds buffer size of 64
			result = io_pwrite(file, buffer, 0, 128, 1)
			
			expect(result).to be == -Errno::EINVAL::Errno
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
