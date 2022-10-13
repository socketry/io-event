# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2022, by Samuel Williams.

require_relative 'selector/select'
require_relative 'debug/selector'
require_relative 'support'

module IO::Event
	module Selector
		def self.default(env = ENV)
			if name = env['IO_EVENT_SELECTOR']&.to_sym
				if const_defined?(name)
					return const_get(name)
				else
					warn "Could not find IO_EVENT_SELECTOR=#{name}!"
				end
			end
			
			if self.const_defined?(:URing)
				URing
			elsif self.const_defined?(:EPoll)
				EPoll
			elsif self.const_defined?(:KQueue)
				KQueue
			else
				Select
			end
		end
		
		def self.new(loop, env = ENV)
			selector = default(env).new(loop)
			
			if debug = env['IO_EVENT_DEBUG_SELECTOR']
				selector = Debug::Selector.new(selector)
			end
			
			return selector
		end
	end
end
