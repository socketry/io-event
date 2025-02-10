// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include "array.h"
#include "time.h"

extern VALUE IO_Event_Profiler;

struct IO_Event_Profiler_Call {
	struct timespec enter_time;
	struct timespec exit_time;
	
	size_t nesting;
	
	rb_event_flag_t event_flag;
	ID id;
	
	VALUE klass;
	const char *path;
	int line;
	
	struct IO_Event_Profiler_Call *parent;
};

struct IO_Event_Profiler {
	VALUE self;
	
	float log_threshold;
	int track_calls;
	
	int running;
	
	struct timespec start_time;
	struct timespec stop_time;
	
	// The depth of the call stack:
	size_t nesting;
	
	// The current call frame:
	struct IO_Event_Profiler_Call *current;
	
	struct IO_Event_Array calls;
};

extern const rb_data_type_t IO_Event_Profiler_Type;
VALUE IO_Event_Profiler_allocate(VALUE klass);
struct IO_Event_Profiler *IO_Event_Profiler_get(VALUE self);
VALUE IO_Event_Profiler_new(float log_threshold, int track_calls);

VALUE IO_Event_Profiler_fiber_transfer(VALUE profiler, VALUE fiber, int argc, VALUE *argv);

void Init_IO_Event_Profiler(VALUE IO_Event);
