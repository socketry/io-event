# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event"
require "io/event/selector"

IOClose = Sus::Shared("io_close") do
	it "can close an IO object" do
		selector = subject.new(Fiber.current)
		next unless selector.respond_to?(:io_close)
		
		input, output = IO.pipe
		
		begin
			expect(selector.io_close(input)).to be_truthy
		ensure
			input.close rescue nil
			output.close rescue nil
			selector.close
		end
	end
	
	# Ruby head/4.1 passes a raw Integer fd to the io_close scheduler hook
	# instead of an IO object. Verify we handle both forms without raising.
	it "can close a raw Integer fd" do
		selector = subject.new(Fiber.current)
		next unless selector.respond_to?(:io_close)
		
		input, output = IO.pipe
		
		# Hand ownership of the fd to io_close so Ruby won't double-close it.
		input.autoclose = false
		fd = input.fileno
		
		begin
			expect(selector.io_close(fd)).to be_truthy
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
	
	describe(klass, unique: name) do
		it_behaves_like IOClose
	end
end
