#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021, by Samuel Williams.

require 'benchmark/ips'
require 'fiber'

require_relative '../lib/event'

GC.disable

Event::Selector.constants.each do |name|
	puts "Creating #{name}..."
	1000.times.map do |i|
		puts i
		selector = Event::Selector.const_get(name).new(Fiber.current)
	end
end
