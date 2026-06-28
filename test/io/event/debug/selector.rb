# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2026, by Samuel Williams.

require "io/event/debug/selector"

require "fiber"
require "stringio"
require "tempfile"

class FakeSelector
	def initialize(loop = Fiber.current)
		@loop = loop
		@closed = false
		@ready = false
		@calls = []
	end
	
	attr :loop
	attr :calls
	
	def idle_duration
		0.1
	end
	
	def wakeup
		:calls_wakeup
	end
	
	def close
		@closed = true
	end
	
	def closed?
		@closed
	end
	
	def transfer
		:calls_transfer
	end
	
	def resume(*arguments)
		@calls << [:resume, arguments]
		:calls_resume
	end
	
	def yield
		:calls_yield
	end
	
	def push(fiber)
		@calls << [:push, fiber]
		:calls_push
	end
	
	def raise(fiber, *arguments, **options)
		@calls << [:raise, fiber, arguments, options]
		:calls_raise
	end
	
	def ready?
		@ready
	end
	
	def blocking_operation_wait(operation)
		@calls << [:blocking_operation_wait, operation]
		:calls_blocking_operation_wait
	end
	
	def process_wait(*arguments)
		@calls << [:process_wait, arguments]
		:calls_process_wait
	end
	
	def io_wait(fiber, io, events)
		@calls << [:io_wait, fiber, io, events]
		:calls_io_wait
	end
	
	def io_read(fiber, io, buffer, length, offset = 0)
		@calls << [:io_read, fiber, io, buffer, length, offset]
		:calls_io_read
	end
	
	def io_write(fiber, io, buffer, length, offset = 0)
		@calls << [:io_write, fiber, io, buffer, length, offset]
		:calls_io_write
	end
	
	def io_close(descriptor)
		@calls << [:io_close, descriptor]
		:calls_io_close
	end
	
	def select(duration = nil)
		@calls << [:select, duration]
		:calls_select
	end
end

describe IO::Event::Debug::Selector do
	it "raises if initialized from a fiber other than the selector loop" do
		selector = FakeSelector.new(Fiber.current)
		
		fiber = Fiber.new do
			subject.new(selector)
		end
		
		expect{fiber.transfer}.to raise_exception(RuntimeError, message: be =~ /initialized/)
	end
	
	it "wraps with a log file" do
		file = Tempfile.new
		selector = subject.wrap(FakeSelector.new, {"IO_EVENT_DEBUG_SELECTOR_LOG" => file.path})
		
		selector.log("Hello")
		selector.close
		selector = nil
		
		expect(File.read(file.path)).to be =~ /Hello/
	ensure
		selector&.close
		file&.close!
	end
	
	it "forwards selector operations" do
		backend = FakeSelector.new
		selector = subject.new(backend, log: StringIO.new)
		fiber = Fiber.current
		input, output = IO.pipe
		buffer = IO::Buffer.new(8)
		
		expect(selector.idle_duration).to be == 0.1
		expect(selector.now).to be_a(Numeric)
		expect(selector.wakeup).to be == :calls_wakeup
		expect(selector.transfer).to be == :calls_transfer
		expect(selector.resume(fiber, :argument)).to be == :calls_resume
		expect(selector.yield).to be == :calls_yield
		expect(selector.push(fiber)).to be == :calls_push
		expect(selector.raise(fiber, "Boom")).to be == :calls_raise
		expect(selector.ready?).to be == false
		expect(selector.closed?).to be == false
		expect(selector.blocking_operation_wait(:operation)).to be == :calls_blocking_operation_wait
		expect(selector.process_wait(123, 0)).to be == :calls_process_wait
		expect(selector.io_wait(fiber, input, IO::READABLE)).to be == :calls_io_wait
		expect(selector.io_read(fiber, input, buffer, 1)).to be == :calls_io_read
		expect(selector.io_write(fiber, output, buffer, 1)).to be == :calls_io_write
		expect(selector.io_close(input.fileno)).to be == :calls_io_close
		expect(selector.respond_to?(:io_close)).to be == true
		expect(selector.select(0)).to be == :calls_select
	ensure
		input&.close
		output&.close
		selector&.close
	end
	
	it "raises if closed twice" do
		selector = subject.new(FakeSelector.new)
		selector.close
		
		expect{selector.close}.to raise_exception(RuntimeError, message: be =~ /already closed/)
	end
	
	it "raises if selected from a fiber other than the selector loop" do
		selector = subject.new(FakeSelector.new(Fiber.current))
		
		fiber = Fiber.new do
			selector.select(0)
		end
		
		expect{fiber.transfer}.to raise_exception(RuntimeError, message: be =~ /run on event loop/)
	ensure
		selector&.close
	end
end
