#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.
#
# Stress test using the ACTUAL Async scheduler (not TestScheduler) to replicate
# the exact Falcon production path. Uses Timeout.timeout wrapping TCPSocket.open
# just like Net::HTTP does, then calls remote_address just like RestrictedTCPSocket.
#
# Usage:
#   ruby test/stress/async_connect.rb [HOST [PORT [CONCURRENCY [TOTAL]]]]
#
# Exit 1 if any ENOTCONN observed.

$LOAD_PATH.unshift(File.expand_path("../../fixtures", __dir__))
$LOAD_PATH.unshift(File.expand_path("../../lib", __dir__))

require "async"
require "io/event"
require "socket"

HOST        = ARGV[0] || "127.0.0.1"
PORT        = (ARGV[1] || 0).to_i  # 0 = local server
CONCURRENCY = (ARGV[2] || 50).to_i
TOTAL       = (ARGV[3] || 5_000).to_i
OPEN_TIMEOUT = (ARGV[4] || 2).to_f  # mirroring Net::HTTP open_timeout

$stderr.puts "Async connect stress test (exact production path)"
$stderr.puts "  Async version   : #{Async::VERSION rescue '?'}"
$stderr.puts "  io-event version: #{IO::Event::VERSION rescue '?'}"
$stderr.puts "  Target          : #{HOST}:#{PORT == 0 ? '(local)' : PORT}"
$stderr.puts "  Concurrency     : #{CONCURRENCY}"
$stderr.puts "  Total           : #{TOTAL}"
$stderr.puts "  open_timeout    : #{OPEN_TIMEOUT}s"
$stderr.puts

counters = Hash.new(0)
mutex    = Mutex.new

# ── local server ───────────────────────────────────────────────────────
if PORT == 0
	server = TCPServer.new("127.0.0.1", 0)
	actual_port = server.addr[1]
	actual_host = "127.0.0.1"
	$stderr.puts "Local server on #{actual_host}:#{actual_port}"
	
	acceptor = Thread.new do
		loop do
			begin
				conn = server.accept_nonblock
				conn.close
			rescue IO::WaitReadable
				IO.select([server])
				retry
			rescue IOError, Errno::EBADF
				break
			end
		end
	end
else
	actual_host = HOST
	actual_port = PORT
end

# ── Async event loop ────────────────────────────────────────────────────
Async do |task|
	TOTAL.times do |i|
		task.async do
			begin
				# Exact Net::HTTP pattern:
				# Timeout.timeout(@open_timeout, Net::OpenTimeout) { TCPSocket.open(...) }
				# followed by socket.remote_address.ip_address
				
				socket = Timeout.timeout(OPEN_TIMEOUT, Errno::ETIMEDOUT) do
					TCPSocket.new(actual_host, actual_port)
				end
				
				# RestrictedTCPSocket.open pattern:
				ip = socket.remote_address.ip_address
				socket.close
				mutex.synchronize { counters[:ok] += 1 }
				
			rescue Errno::ENOTCONN => e
				mutex.synchronize { counters[:ENOTCONN] += 1 }
				$stderr.puts "[ENOTCONN ##{i}] #{e.message}"
			rescue Errno::ECONNREFUSED
				mutex.synchronize { counters[:ECONNREFUSED] += 1 }
			rescue Errno::ECONNRESET
				mutex.synchronize { counters[:ECONNRESET] += 1 }
			rescue Timeout::Error, ::IO::TimeoutError => e
				mutex.synchronize { counters[:timeout] += 1 }
			rescue => e
				mutex.synchronize { counters[e.class] += 1 }
				$stderr.puts "[ERROR] #{e.class}: #{e.message}"
			end
		end
	end
end

server&.close
acceptor&.kill rescue nil

$stderr.puts "\n=== Results ==="
$stderr.puts "  ok           : #{counters[:ok]}"
$stderr.puts "  ENOTCONN     : #{counters[:ENOTCONN]}"
$stderr.puts "  ECONNRESET   : #{counters[:ECONNRESET]}"
$stderr.puts "  ECONNREFUSED : #{counters[:ECONNREFUSED]}"
$stderr.puts "  timeout      : #{counters[:timeout]}"
other = counters.reject { |k,_| [:ok,:ENOTCONN,:ECONNRESET,:ECONNREFUSED,:timeout].include?(k) }
$stderr.puts "  other        : #{other.inspect}" unless other.empty?

if counters[:ENOTCONN] > 0
	$stderr.puts "\nREPRODUCED: #{counters[:ENOTCONN]} ENOTCONN errors!"
	exit 1
else
	$stderr.puts "\nNo ENOTCONN observed."
	exit 0
end
