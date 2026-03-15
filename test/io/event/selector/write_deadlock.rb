# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

WriteDeadlock = Sus::Shared("write deadlock") do
	with "a pipe that fills up" do
		it "should not deadlock when waiting for writable" do
			# Skip on Windows which doesn't have the same socket behavior
			skip_if_ruby_platform(/mswin|mingw|cygwin/)
			
			# Use UNIXSocket pair for more predictable behavior
			local, remote = UNIXSocket.pair(:STREAM)
			
			# Set small buffer to encourage EAGAIN
			local.setsockopt(Socket::SOL_SOCKET, Socket::SO_SNDBUF, 4096)
			remote.setsockopt(Socket::SOL_SOCKET, Socket::SO_RCVBUF, 4096)
			
			eagain_hit = false
			write_completed = false
			
			# Fill buffer until we actually hit EAGAIN
			begin
				chunk = "X" * 1024  # 1KB chunks
				100.times{local.write_nonblock(chunk)}  # Write up to 100KB
			rescue IO::WaitWritable
				eagain_hit = true
			end
			
			# Skip test if we can't create EAGAIN condition
			skip "Could not trigger EAGAIN condition" unless eagain_hit
			
			# Writer fiber that should hit EAGAIN and wait for WRITABLE
			writer = Fiber.new do
				buffer = IO::Buffer.for("test" * 64)  # 256 bytes
				@selector.io_write(Fiber.current, local, buffer, buffer.size, 0)
				write_completed = true
			end
			
			# Start writer - should yield back when hitting EAGAIN
			writer.transfer
			
			# Writer should be stuck waiting (either for right or wrong event)
			expect(writer.alive?).to be == true
			expect(write_completed).to be == false
			
			# Drain some data to make socket writable
			remote.read_nonblock(4096)
			
			# Give selector multiple chances to process writable event.
			# With fix: writer should wake up and complete.
			# With bug: writer stays stuck because it's waiting for READABLE.
			timeout_count = 0
			while writer.alive? && timeout_count < 10
				@selector.select(1.0)  # Short intervals for responsiveness, many iterations for tolerance
				timeout_count += 1
			end
			
			expect(write_completed).to be == true
			expect(writer).not.to be(:alive?)
		ensure
			local.close rescue nil
			remote.close rescue nil
		end
	end
end

# Test all available selectors
IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		def before
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		def after(error = nil)
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like WriteDeadlock
	end
end
