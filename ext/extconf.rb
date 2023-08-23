#!/usr/bin/env ruby
# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2021-2023, by Samuel Williams.
# Copyright, 2023, by Math Ieu.

return if RUBY_DESCRIPTION =~ /jruby/

require 'mkmf'

gem_name = File.basename(__dir__)
extension_name = 'IO_Event'

# dir_config(extension_name)

$CFLAGS << " -Wall -Wno-unknown-pragmas -std=c99"

if ENV.key?('RUBY_DEBUG')
	$CFLAGS << " -DRUBY_DEBUG -O0"
end

$srcs = ["io/event/event.c", "io/event/selector/selector.c"]
$VPATH << "$(srcdir)/io/event"
$VPATH << "$(srcdir)/io/event/selector"

have_func('rb_ext_ractor_safe')
have_func('&rb_fiber_transfer')

if have_library('uring') and have_header('liburing.h')
	$srcs << "io/event/selector/uring.c"
end

if have_header('sys/epoll.h')
	$srcs << "io/event/selector/epoll.c"
end

if have_header('sys/event.h')
	$srcs << "io/event/selector/kqueue.c"
end

have_header('sys/eventfd.h')
$srcs << "io/event/interrupt.c"

have_func("rb_io_descriptor")
have_func("&rb_process_status_wait")
have_func("rb_fiber_current")
have_func("&rb_fiber_raise")
have_func("epoll_pwait2")

have_header('ruby/io/buffer.h')

create_header

# Generate the makefile to compile the native binary into `lib`:
create_makefile(extension_name)
