# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2022-2024, by Samuel Williams.

require "io/event"
require "io/nonblock"
require "io/event/selector"

require "socket"
require "unix_socket"

describe IO::Event::Selector do
	with ".nonblock" do
		it "makes non-blocking IO" do
			executed = false
			
			input, output = UNIXSocket.pair
			
			begin
				input.nonblock = false
				output.nonblock = false
				
				IO::Event::Selector.nonblock(input) do
					executed = true
					
					# This does not work on Windows; JRuby's `nonblock?` does not
					# reliably reflect the temporary block state for this socket.
					unless RUBY_PLATFORM =~ /mswin|mingw|cygwin/ or RUBY_ENGINE == "jruby"
						expect(input).to be(:nonblock?)
						expect(output).not.to be(:nonblock?)
					end
				end
			ensure
				input&.close unless input&.closed?
				output&.close unless output&.closed?
			end
			
			expect(executed).to be == true
		end
	end
end
