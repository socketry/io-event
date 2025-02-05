// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include "array.h"
#include "time.h"

extern VALUE IO_Event_Profile;

struct IO_Event_Profile_Call {
	struct timespec enter_time;
	struct timespec exit_time;
	
	size_t nesting;
	
	rb_event_flag_t event_flag;
	ID id;
	
	VALUE klass;
	const char *path;
	int line;
	
	struct IO_Event_Profile_Call *parent;
};

struct IO_Event_Profile {
	int track_calls;
	
	struct timespec start_time;
	struct timespec stop_time;
	
	// The depth of the call stack:
	size_t nesting;
	
	// The current call frame:
	struct IO_Event_Profile_Call *current;
	
	struct IO_Event_Array calls;
};

extern const rb_data_type_t IO_Event_Profile_Type;
VALUE IO_Event_Profile_allocate(VALUE klass);
struct IO_Event_Profile *IO_Event_Profile_get(VALUE self);

void IO_Event_Profile_initialize(struct IO_Event_Profile *profile, VALUE fiber);
void IO_Event_Profile_start(VALUE self, int track_calls);
void IO_Event_Profile_stop(VALUE self);

void IO_Event_Profile_print(VALUE profile, FILE *restrict stream);

static inline float IO_Event_Profile_duration(VALUE self) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(self);
	
	struct timespec duration;
	
	IO_Event_Time_elapsed(&profile->start_time, &profile->stop_time, &duration);
	
	return IO_Event_Time_duration(&duration);
}

void Init_IO_Event_Profile(VALUE IO_Event);
