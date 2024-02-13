# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require 'io/event'
require 'io/event/selector'
require 'fileutils'
require 'tmpdir'

FifoIO = Sus::Shared("fifo io") do
	with 'a fifo' do
		def around
			@root = Dir.mktmpdir
			super
		ensure
			FileUtils.rm_rf(@root) if @root
		end
		
		let(:path) {File.join(@root, "fifo")}
		
		it 'can read and write' do
			File.mkfifo(path)
			input = File.open(path, "r+")
			output = File.open(path, "w+")
			
			buffer = IO::Buffer.new(128)
			
			reader = Fiber.new do
				@selector.io_wait(Fiber.current, input, IO::READABLE)
				result = buffer.read(input, buffer.size)
				buffer.resize(result)
			end
			
			writer = Fiber.new do
				output.puts("Hello World\n")
				output.close
			end
			
			reader.transfer
			writer.transfer
			
			2.times do
				@selector.select(0)
			end
			
			expect(buffer.get_string).to be == "Hello World\n"
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	describe(klass, unique: name) do
		def before
			@loop = Fiber.current
			@selector = subject.new(@loop)
		end
		
		def after
			@selector&.close
		end
		
		attr :loop
		attr :selector
		
		it_behaves_like FifoIO
	end
end
