# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.

require_relative 'scheduler'

RSpec.shared_examples_for IO::Event::Scheduler do
	subject(:scheduler) {IO::Event::Scheduler.new(selector)}
	
	around do |example|
		thread = Thread.new do
			Fiber.set_scheduler(scheduler)
			example.run
		end
		
		thread.join
	end
	
	it 'can run several fibers' do
		sum = 0
		
		fibers = 3.times.map do |i|
			Fiber.schedule{sleep 0.001; sum += i}
		end
		
		subject.run
		
		expect(sum).to be == 3
	end
	
	it 'can join threads' do
		Fiber.schedule do
			1000.times do
				thread = ::Thread.new do
					sleep(0.001)
				end
				
				thread.join(0.001)
			ensure
				thread&.join
			end
		end
	end
end

IO::Event::Selector.constants.each do |name|
	klass = IO::Event::Selector.const_get(name)
	
	RSpec.describe(klass) do
		let(:loop) {Fiber.current}
		let(:selector){described_class.new(loop)}
		
		it_behaves_like IO::Event::Scheduler
	end
end
