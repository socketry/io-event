// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.
// DEBUG STUB v2: no winsock includes — just kernel32 and Ruby APIs.

#include "iocp.h"
#include "selector.h"

#ifdef _WIN32

#include <ruby/win32.h>
#include <windows.h>

VALUE IO_Event_Selector_IOCP_allocate(VALUE self) { return self; }
VALUE IO_Event_Selector_IOCP_initialize(VALUE self, VALUE loop) { return self; }

void Init_IO_Event_Selector_IOCP(VALUE IO_Event_Selector) {
	VALUE klass = rb_define_class_under(IO_Event_Selector, "IOCP", rb_cObject);
	rb_define_alloc_func(klass, IO_Event_Selector_IOCP_allocate);
	rb_define_method(klass, "initialize", IO_Event_Selector_IOCP_initialize, 1);
}

#endif // _WIN32
