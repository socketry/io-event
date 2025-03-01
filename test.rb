#!/usr/bin/env ruby

require "async"


Async do
	Fiber.scheduler.yield
	
	Fiber.blocking do
		sleep 2
	end
end
