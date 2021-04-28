#!/usr/bin/env ruby

require 'benchmark/ips'
require 'fiber'

require_relative '../lib/event'

GC.disable

Event::Backend.constants.each do |name|
	puts "Creating #{name}..."
	1000.times.map do |i|
		puts i
		backend = Event::Backend.const_get(name).new(Fiber.current)
	end
end