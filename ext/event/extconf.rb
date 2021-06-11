#!/usr/bin/env ruby

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

require 'mkmf'

gem_name = File.basename(__dir__)
extension_name = 'event'

# The destination
dir_config(extension_name)

$CFLAGS << " -Wall"

$srcs = ["event.c", "backend/backend.c"]
$VPATH << "$(srcdir)/backend"

have_func('rb_fiber_transfer_kw')

if have_library('uring') and have_header('liburing.h')
	$srcs << "backend/uring.c"
end

if have_header('sys/epoll.h')
	$srcs << "backend/epoll.c"
end

if have_header('sys/event.h')
	$srcs << "backend/kqueue.c"
end

create_header

# Generate the makefile to compile the native binary into `lib`:
create_makefile(File.join(gem_name, extension_name))
