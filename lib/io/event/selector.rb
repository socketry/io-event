# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2026, by Samuel Williams.

require_relative "native"
require_relative "selector/nonblock"
require_relative "selector/select"
require_relative "debug/selector"

module IO::Event
	# @namespace
	module Selector
		selectors = [:URing, :EPoll, :KQueue, :Select]
		BEST = const_get(selectors.find{|name| const_defined?(name)})
		private_constant :BEST
		
		# The default selector implementation, which is chosen based on the environment and available implementations.
		#
		# @parameter env [Hash] The environment to read configuration from.
		# @returns [Class] The default selector implementation.
		def self.default(env = ENV)
			if name = env["IO_EVENT_SELECTOR"]&.to_sym
				return const_get(name)
			else
				BEST
			end
		end
		
		# Create a new selector instance, according to the best available implementation.
		#
		# @parameter loop [Fiber] The event loop fiber.
		# @parameter env [Hash] The environment to read configuration from.
		# @returns [Selector] The new selector instance.
		def self.new(loop, env = ENV)
			selector = default(env).new(loop)
			
			if debug = env["IO_EVENT_DEBUG_SELECTOR"]
				selector = Debug::Selector.wrap(selector, env)
			end
			
			return selector
		end
		
		# Wait for a process to change state, for the cases a selector cannot represent natively (e.g. `pid <= 0`: any child, or a process group). The native selectors integrate process waiting with the event loop using per-process primitives (`pidfd_open`, `EVFILT_PROC`) which can only refer to a single, specific process, and delegate here otherwise.
		#
		# The wait is performed on a separate thread, which has no fiber scheduler and therefore blocks. Joining it via `Thread#value` is fiber-scheduler aware, so the calling fiber yields to the event loop and the reactor keeps running other fibers.
		#
		# @parameter pid [Integer] The process ID (or process group) to wait for.
		# @parameter flags [Integer] Flags to pass to `Process::Status.wait`.
		# @returns [Process::Status] The status of the waited process.
		def self.process_wait(pid, flags)
			thread = ::Thread.new do
				::Process::Status.wait(pid, flags)
			end
			
			thread.value
		ensure
			# If the calling fiber was interrupted before the wait completed, don't leave the thread running:
			thread&.kill
		end
	end
end
