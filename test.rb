#!/usr/bin/env ruby

r, w = IO.pipe

thread = Thread.new do
	# pp IO.select([r])
	pp r.wait_readable
rescue => error
	pp error
end

Thread.pass until thread.status == "sleep"

r.close

binding.irb
