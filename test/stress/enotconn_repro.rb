#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

# Pure-Ruby ENOTCONN reproduction test.
#
# Replicates the exact production code path:
#   Net::HTTP → Timeout.timeout → TCPSocket.open → socket.remote_address.ip_address
#
# Two modes:
#   Local (default):  loopback server that RSTs immediately via SO_LINGER(0).
#                     This races TCP_CLOSE against getpeername — we can reproduce
#                     ~4 ENOTCONN per 10 000 connections on kernel 6.19.
#
#   Remote (-r HOST:PORT): connect to a real endpoint.
#                     On GKE/COS 6.12 with io-event 1.14.0 this is the
#                     highest-fidelity reproduction of the production bug.
#
# Usage:
#   ruby enotconn_repro.rb [-r HOST:PORT] [-n TOTAL] [-c CONCURRENCY] [-t TIMEOUT]
#
# Exit 0 if no ENOTCONN, 1 if any observed.

require "optparse"

options = {
  remote: nil,
  total: 5_000,
  concurrency: 100,
  timeout: 2.0,
}

OptionParser.new do |o|
  o.on("-r", "--remote HOST:PORT") { |v| options[:remote] = v }
  o.on("-n", "--total N", Integer) { |v| options[:total] = v }
  o.on("-c", "--concurrency N", Integer) { |v| options[:concurrency] = v }
  o.on("-t", "--timeout SECS", Float) { |v| options[:timeout] = v }
end.parse!

# ── setup ──────────────────────────────────────────────────────────────

$LOAD_PATH.unshift(File.expand_path("../../lib", __dir__))
$LOAD_PATH.unshift(File.expand_path("../../fixtures", __dir__))

require "async"
require "io/event"
require "socket"

LINGER_RST = [1, 0].pack("ii")  # SO_LINGER: l_onoff=1, l_linger=0 → sends RST

if options[:remote]
  host, port = options[:remote].split(":")
  target_host = host
  target_port = port.to_i
  server = nil
  acceptor = nil
  $stderr.puts "Remote target: #{target_host}:#{target_port}"
else
  srv = TCPServer.new("127.0.0.1", 0)
  target_host = "127.0.0.1"
  target_port = srv.addr[1]
  # RST-close server: creates the TCP_CLOSE race against getpeername
  acceptor = Thread.new do
    loop do
      begin
        conn = srv.accept_nonblock
        conn.setsockopt(Socket::SOL_SOCKET, Socket::SO_LINGER, LINGER_RST)
        conn.close
      rescue IO::WaitReadable
        IO.select([srv])
        retry
      rescue IOError, Errno::EBADF
        break
      end
    end
  end
  server = srv
  $stderr.puts "Local RST server: #{target_host}:#{target_port}"
end

$stderr.puts "Scheduler: #{Async::VERSION rescue '?'}  io-event: #{IO::Event::VERSION rescue '?'}"
$stderr.puts "Kernel:    #{`uname -r`.strip rescue '?'}"
$stderr.puts "Total: #{options[:total]}  Concurrency: #{options[:concurrency]}  Timeout: #{options[:timeout]}s"
$stderr.puts

# ── run ────────────────────────────────────────────────────────────────

counters = Hash.new(0)
mutex = Mutex.new

Async do |task|
  options[:total].times do |i|
    task.async do
      begin
        # Exact Net::HTTP#timeouted_connect pattern:
        socket = Timeout.timeout(options[:timeout], Errno::ETIMEDOUT) do
          TCPSocket.new(target_host, target_port)
        end

        # Exact RestrictedTCPSocket.open pattern that raises ENOTCONN:
        ip = socket.remote_address&.ip_address
        socket.close
        mutex.synchronize { counters[:ok] += 1 }

      rescue Errno::ENOTCONN => e
        mutex.synchronize { counters[:ENOTCONN] += 1 }
        $stderr.puts "[#{counters[:ENOTCONN]}] ENOTCONN at attempt #{i}: #{e.message}"
      rescue Errno::ECONNRESET  then mutex.synchronize { counters[:ECONNRESET] += 1 }
      rescue Errno::ECONNREFUSED then mutex.synchronize { counters[:ECONNREFUSED] += 1 }
      rescue Errno::ETIMEDOUT,
             Timeout::Error,
             ::IO::TimeoutError then mutex.synchronize { counters[:timeout] += 1 }
      rescue => e
        mutex.synchronize { counters[e.class] += 1 }
        $stderr.puts "[ERROR] #{e.class}: #{e.message}"
      end
    end
  end
end

# ── results ────────────────────────────────────────────────────────────

server&.close
acceptor&.kill rescue nil

$stderr.puts "\n=== Results ==="
$stderr.puts "  ok           : #{counters[:ok]}"
$stderr.puts "  ENOTCONN     : #{counters[:ENOTCONN]}  ← root cause if > 0"
$stderr.puts "  ECONNRESET   : #{counters[:ECONNRESET]}"
$stderr.puts "  ECONNREFUSED : #{counters[:ECONNREFUSED]}"
$stderr.puts "  timeout      : #{counters[:timeout]}"
other = counters.reject { |k,_| [:ok,:ENOTCONN,:ECONNRESET,:ECONNREFUSED,:timeout].include?(k) }
$stderr.puts "  other        : #{other.inspect}" unless other.empty?

if counters[:ENOTCONN] > 0
  $stderr.puts "\nREPRODUCED: #{counters[:ENOTCONN]} ENOTCONN error(s). Bug confirmed on this kernel/config."
  exit 1
else
  $stderr.puts "\nNo ENOTCONN observed."
  exit 0
end
