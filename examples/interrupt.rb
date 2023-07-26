#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require 'fiber'
require '../lib/event'

th = Thread.new do
	selector = Event::Selector.new(Fiber.current)
	$stderr.puts "select"
	selector.select(10)
	$stderr.puts "select done"
ensure
	$stderr.puts "exiting: #{$!}"
end

sleep 1
$stderr.puts "Sending interrupt"
th.wakeup

th.join

# 
# c = Thread.new { Thread.stop; puts "hey!" }
# sleep 0.1 while c.status!='sleep'
# c.wakeup
# c.join
# #=> "hey!"
