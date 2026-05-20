#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

# Stress test for the ENOTCONN / io_wait ZERO bug.
#
# Usage:
#   ruby test/stress/tcp_connect.rb [HOST [PORT [CONCURRENCY [TOTAL]]]]
#
# Defaults to connecting to a real external host so that the connect goes
# through EINPROGRESS with genuine network latency — the condition absent
# from loopback-only tests.
#
# If DEBUG_IO_WAIT=1 is compiled in, look at stderr for ZERO lines.
# Exit 1 on any ENOTCONN.

$LOAD_PATH.unshift(File.expand_path("../../fixtures", __dir__))

require "io/event"
require "io/event/test_scheduler"
require "socket"

unless IO::Event::Selector.const_defined?(:URing)
	abort "ERROR: URing backend not available."
end

HOST        = ARGV[0] || "127.0.0.1"
PORT        = (ARGV[1] || 0).to_i   # 0 = let OS pick
CONCURRENCY = (ARGV[2] || 100).to_i
TOTAL       = (ARGV[3] || 5_000).to_i
# Delay (ms) the server sleeps before accepting, simulating real network RTT.
# Default 5ms — enough to ensure poll_add is submitted while connect is in flight.
# 0 = immediate RST (SO_LINGER l_linger=0); any positive value = clean FIN after N ms
DELAY_MS    = (ARGV[4] || 0).to_i
RST_CLOSE   = DELAY_MS == 0
PER_FIBER   = (TOTAL.to_f / CONCURRENCY).ceil

$stderr.puts "io-event ENOTCONN stress test"
$stderr.puts "  Backend     : #{IO::Event::Selector::URing}"
$stderr.puts "  Concurrency : #{CONCURRENCY} fibers"
$stderr.puts "  Total       : #{TOTAL} connections (#{PER_FIBER} per fiber)"
$stderr.puts "  Server close: #{RST_CLOSE ? "immediate RST (SO_LINGER=0)" : "clean FIN after #{DELAY_MS}ms"}"
$stderr.puts

# ── server with configurable accept delay ───────────────────────────────────

server = TCPServer.new("127.0.0.1", PORT)
actual_port = server.addr[1]
$stderr.puts "Server on 127.0.0.1:#{actual_port}"

acceptor_thread = Thread.new do
	loop do
		begin
			conn = server.accept_nonblock
			if RST_CLOSE
				# SO_LINGER(0) sends RST immediately — socket transitions
				# to TCP_CLOSE on the client before getpeername can run.
				conn.setsockopt(Socket::SOL_SOCKET, Socket::SO_LINGER,
					[1, 0].pack("ii"))
			else
				sleep(DELAY_MS / 1000.0)
			end
			conn.close
		rescue IO::WaitReadable
			IO.select([server])
			retry
		rescue IOError, Errno::EBADF
			break
		rescue => e
			$stderr.puts "[acceptor] #{e.class}: #{e.message}"
		end
	end
end

counters = Hash.new(0)
mutex    = Mutex.new

selector  = IO::Event::Selector::URing.new(Fiber.current)
scheduler = IO::Event::TestScheduler.new(selector: selector)

Fiber.set_scheduler(scheduler)

CONCURRENCY.times do
	Fiber.schedule do
		PER_FIBER.times do
			begin
				sock = TCPSocket.new("127.0.0.1", actual_port)
				# Immediately try remote_address — exactly what
				# shopify_security_base does and where ENOTCONN fires.
				_ = sock.remote_address.ip_address
				sock.close
				mutex.synchronize { counters[:ok] += 1 }
			rescue Errno::ENOTCONN => e
				mutex.synchronize { counters[:ENOTCONN] += 1 }
				$stderr.puts "[ENOTCONN] #{e.message}"
			rescue Errno::ECONNREFUSED
				mutex.synchronize { counters[:ECONNREFUSED] += 1 }
			rescue Errno::ECONNRESET
				mutex.synchronize { counters[:ECONNRESET] += 1 }
			rescue Errno::ETIMEDOUT
				mutex.synchronize { counters[:ETIMEDOUT] += 1 }
			rescue => e
				mutex.synchronize { counters[e.class] += 1 }
				$stderr.puts "[ERROR] #{e.class}: #{e.message}"
			end
		end
	end
end

scheduler.run
Fiber.set_scheduler(nil)

server.close
acceptor_thread.join(2)

$stderr.puts
$stderr.puts "=== Results ==="
$stderr.puts "  Successful  : #{counters[:ok]}"
$stderr.puts "  ENOTCONN    : #{counters[:ENOTCONN]}"
$stderr.puts "  ECONNREFUSED: #{counters[:ECONNREFUSED]}"
$stderr.puts "  ECONNRESET  : #{counters[:ECONNRESET]}"
$stderr.puts "  ETIMEDOUT   : #{counters[:ETIMEDOUT]}"
other = counters.reject { |k,_| [:ok,:ENOTCONN,:ECONNREFUSED,:ECONNRESET,:ETIMEDOUT].include?(k) }
$stderr.puts "  Other       : #{other.inspect}" unless other.empty?

if counters[:ENOTCONN] > 0
	$stderr.puts "\nREPRODUCED: #{counters[:ENOTCONN]} ENOTCONN error(s) observed."
	exit 1
else
	$stderr.puts "\nNo ENOTCONN observed."
	exit 0
end
