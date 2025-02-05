// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include "array.h"
#include "time.h"

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
	VALUE fiber;
	
	struct timespec start_time;
	struct timespec stop_time;
	
	// The depth of the call stack:
	size_t nesting;
	
	// The current call frame:
	struct IO_Event_Profile_Call *current;
	
	struct IO_Event_Array calls;
};

void IO_Event_Profile_initialize(struct IO_Event_Profile *profile, VALUE fiber);

void IO_Event_Profile_start(struct IO_Event_Profile *profile);
void IO_Event_Profile_stop(struct IO_Event_Profile *profile);

void IO_Event_Profile_free(struct IO_Event_Profile *profile);
void IO_Event_Profile_print(FILE *restrict stream, struct IO_Event_Profile *profile);

static inline float IO_Event_Profile_duration(struct IO_Event_Profile *profile) {
	struct timespec duration;
	
	IO_Event_Time_elapsed(&profile->start_time, &profile->stop_time, &duration);
	
	return IO_Event_Time_duration(&duration);
}
