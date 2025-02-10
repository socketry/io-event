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
	float log_threshold;
	int track_calls;
	
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

void IO_Event_Profiler_print(VALUE profiler, FILE *restrict stream);

void Init_IO_Event_Profiler(VALUE IO_Event);

// Track entry into a fiber (scheduling operation).
void IO_Event_Profiler_enter(VALUE self, VALUE fiber);

// Track exit from a fiber (scheduling operation).
void IO_Event_Profiler_exit(VALUE self, VALUE fiber);
