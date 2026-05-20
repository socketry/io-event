# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "socket"

describe IO::Event::Selector do
	with "URing" do
		it "does not resume io_wait with zero for an unrequested priority event" do
			skip "URing is not available" unless subject.const_defined?(:URing)

			uring = subject.const_get(:URing)
			selector = uring.new(Fiber.current)

			server = TCPServer.new("127.0.0.1", 0)
			client = TCPSocket.new("127.0.0.1", server.addr[1])
			peer = server.accept

			result = nil

			fiber = Fiber.new do
				result = selector.io_wait(Fiber.current, client, IO::READABLE)
			end

			fiber.transfer

			peer.send("!", Socket::MSG_OOB)
			selector.select(0.1)

			expect(result).not.to be == 0

			if fiber.alive?
				peer.write("x")

				10.times do
					selector.select(0.1)
					break unless fiber.alive?
				end

				expect(result).to be == IO::READABLE
			end
		ensure
			selector&.close
			client&.close
			peer&.close
			server&.close
		end

		it "does not expose unmatched poll completions as integer zero from io_wait" do
			skip "URing is not available" unless subject.const_defined?(:URing)
			
			uring = subject.const_get(:URing)
			
			# io_uring poll completions can include flags we did not request. The
			# current implementation filters the raw poll result before translating
			# it to Ruby IO events, which can produce Integer(0). Ruby's socket
			# connect path treats any non-false, non-negative result as success.
			result = uring.__test_io_wait_unmatched_poll_result(IO::READABLE | IO::WRITABLE)
			
			expect(result).not.to be == 0
		end
	end
end
