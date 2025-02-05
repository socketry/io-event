# Releases

## v1.8.0

### Detecing fibers that are stalling the event loop.

A new (experimental) feature for detecting fiber stalls has been added. This feature is disabled by default and can be enabled by setting the `IO_EVENT_SELECTOR_STALL_LOG_THRESHOLD` to `true` or a floating point number representing the threshold in seconds.

When enabled, the event loop will measure and profile user code when resuming a fiber. If the fiber takes too long to return back to the event loop, the event loop will log a warning message with a profile of the fiber's execution.

    > cat test.rb 
    #!/usr/bin/env ruby
    
    require_relative "lib/async"
    
    Async do
    	Fiber.blocking do
    		sleep 1
    	end
    end
    
    > IO_EVENT_SELECTOR_STALL_LOG_THRESHOLD=true bundle exec ./test.rb
    Fiber stalled for 1.003 seconds
    	/home/samuel/Developer/socketry/async/test.rb:6 in '#<Class:Fiber>#blocking' (1s)
    		/home/samuel/Developer/socketry/async/test.rb:7 in 'Kernel#sleep' (1s)

There is a performance overhead to this feature, so it is recommended to only enable it when debugging performance issues.

## v1.7.5

  - Fix `process_wait` race condition on EPoll that could cause a hang.
