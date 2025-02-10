# Releases

## v1.9.0

### Improved `IO::Event::Profiler` for detecting stalls.

A new `IO::Event::Profiler` class has been added to help detect stalls in the event loop. The previous approach was insufficient to detect all possible stalls. This new approach uses the `RUBY_EVENT_FIBER_SWITCH` event to track context switching by the scheduler, and can detect stalls no matter how they occur.

``` ruby
profiler = IO::Event::Profiler.new

profiler.start
		
Fiber.new do
	sleep 1.0
end.transfer

profiler.stop
```

A default profiler is exposed using `IO::Event::Profiler.default` which is controlled by the following environment variables:

  - `IO_EVENT_PROFILER=true` - Enable the profiler, otherwise `IO::Event::Profiler.default` will return `nil`.
  - `IO_EVENT_PROFILER_LOG_THRESHOLD` - Specify the threshold in seconds for logging a stall. Defaults to `0.01`.
  - `IO_EVENT_PROFILER_TRACK_CALLS` - Track the method call for each event, in order to log specifically which method is causing the stall. Defaults to `true`.

The previous environment variables `IO_EVENT_SELECTOR_STALL_LOG_THRESHOLD` and `IO_EVENT_SELECTOR_STALL_LOG` no longer have any effect.

## v1.8.0

### Detecting fibers that are stalling the event loop.

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
