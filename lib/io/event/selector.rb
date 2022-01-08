# Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

require_relative 'selector/select'
require_relative 'debug/selector'

module IO::Event
	module Selector
		def self.default(env = ENV)
			if name = env['IO_EVENT_SELECTOR']&.to_sym
				if const_defined?(name)
					return const_get(name)
				else
					warn "Could not find IO_EVENT_SELECTOR=#{name}!"
				end
			end
			
			if self.const_defined?(:EPoll)
				return EPoll
			elsif self.const_defined?(:KQueue)
				return KQueue
			else
				return Select
			end
		end
		
		def self.new(loop, env = ENV)
			selector = default(env).new(loop)
			
			if debug = env['IO_EVENT_DEBUG_SELECTOR']
				selector = Debug::Selector.new(selector)
			end
			
			return selector
		end
	end
end
