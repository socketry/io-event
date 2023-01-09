# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022, by Samuel Williams.

require 'io/event'
require 'io/nonblock'
require 'io/event/selector'

describe IO::Event::Selector do
	with '.nonblock' do
		it "makes non-blocking IO" do
			executed = false
			
			UNIXSocket.pair do |input, output|
				input.nonblock = false
				output.nonblock = false
				
				IO::Event::Selector.nonblock(input) do
					executed = true
					
					# This does not work on Windows...
					unless RUBY_PLATFORM =~ /mswin|mingw|cygwin/
						expect(input).to be(:nonblock?)
						expect(output).not.to be(:nonblock?)
					end
				end
			end
			
			expect(executed).to be == true
		end
	end
end
