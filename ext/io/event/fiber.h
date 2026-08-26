// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>

VALUE IO_Event_Fiber_transfer(VALUE fiber, int argc, VALUE *argv);

#define IO_Event_Fiber_raise(fiber, argc, argv) rb_fiber_raise(fiber, argc, argv)
#define IO_Event_Fiber_current() rb_fiber_current()

int IO_Event_Fiber_blocking(VALUE fiber);
void Init_IO_Event_Fiber(VALUE IO_Event);
