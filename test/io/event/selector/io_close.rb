# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"
require "io/event/debug/selector"

# Ruby invokes the `io_close` fiber-scheduler hook with a raw integer file descriptor (Ruby 4.0+, see `rb_fiber_scheduler_io_close` in CRuby). Verify each selector that opts into the hook handles that contract.
IOClose = Sus::Shared("io_close") do
	it "can close a raw file descriptor" do
		selector = subject.new(Fiber.current)
		
		input, output = IO.pipe
		
		# Hand ownership of the fd to `io_close` so Ruby won't double-close it.
		input.autoclose = false
		descriptor = input.fileno
		
		begin
			expect(selector.io_close(descriptor)).to be_truthy
		ensure
			input.close rescue nil
			output.close rescue nil
			selector.close
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	next unless klass.respond_to?(:new)
	next unless klass.method_defined?(:io_close)
	
	describe(klass, unique: name) do
		it_behaves_like IOClose
	end
	
	# `Debug::Selector` should transparently forward `io_close` to any wrapped selector that implements it (see `Forwarders`). The shared examples build the selector via `subject.new(loop)`, so we hand them a thin factory that closes over the underlying selector class.
	debug_class = Class.new do
		define_singleton_method(:new) do |loop|
			IO::Event::Debug::Selector.new(klass.new(loop))
		end
	end
	
	describe(debug_class, unique: "Debug(#{name})") do
		it_behaves_like IOClose
	end
end
