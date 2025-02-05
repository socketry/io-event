// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profile.h"
#include "time.h"

#include <ruby/debug.h>

#include <stdio.h>

void IO_Event_Profile_Call_initialize(struct IO_Event_Profile_Call *event) {
	event->enter_time.tv_sec = 0;
	event->enter_time.tv_nsec = 0;
	event->exit_time.tv_sec = 0;
	event->exit_time.tv_nsec = 0;
	
	event->nesting = 0;
	
	event->event_flag = 0;
	event->id = 0;
	
	event->path = NULL;
	event->line = 0;
}

void IO_Event_Profile_Call_free(struct IO_Event_Profile_Call *event) {
	if (event->path) {
		free((void*)event->path);
	}
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN);
}

static void profile_event_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct IO_Event_Profile *profile = (struct IO_Event_Profile*)data;
	
	if (event_flag_call_p(event_flag)) {
		struct IO_Event_Profile_Call *event = IO_Event_Array_push(&profile->events);
		IO_Event_Time_current(&event->enter_time);
	
		event->event_flag = event_flag;
		
		event->parent = profile->current;
		profile->current = event;
		
		event->nesting = profile->nesting;
		profile->nesting += 1;
		
		if (id) {
			event->id = id;
			event->klass = klass;
		} else {
			rb_frame_method_id_and_class(&event->id, &event->klass);
		}
		
		const char *path = rb_sourcefile();
		if (path) {
			event->path = strdup(path);
		}
		event->line = rb_sourceline();
	} else if (event_flag_return_p(event_flag)) {
		struct IO_Event_Profile_Call *event = profile->current;
		
		// Bad event sequence?
		if (event == NULL) return;
		
		IO_Event_Time_current(&event->exit_time);
		
		profile->current = event->parent;
		profile->nesting -= 1;
	}
}

void IO_Event_Profile_initialize(struct IO_Event_Profile *profile, VALUE fiber) {
	profile->fiber = fiber;
	
	profile->events.element_initialize = (void (*)(void*))IO_Event_Profile_Call_initialize;
	profile->events.element_free = (void (*)(void*))IO_Event_Profile_Call_free;
	
	IO_Event_Array_initialize(&profile->events, 0, sizeof(struct IO_Event_Profile_Call));
}

void IO_Event_Profile_start(struct IO_Event_Profile *profile) {
	IO_Event_Time_current(&profile->start_time);
	profile->nesting = 0;
	profile->current = NULL;
	
	// Since fibers are currently limited to a single thread, we use this in the hope that it's a little more efficient:
	VALUE thread = rb_thread_current();
	rb_thread_add_event_hook(thread, profile_event_callback, RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN, (VALUE)profile);
}

void IO_Event_Profile_stop(struct IO_Event_Profile *profile) {
	IO_Event_Time_current(&profile->stop_time);
	
	VALUE thread = rb_thread_current();
	rb_thread_remove_event_hook_with_data(thread, profile_event_callback, (VALUE)profile);
}

void IO_Event_Profile_free(struct IO_Event_Profile *profile) {
	IO_Event_Array_free(&profile->events);
}

static const float IO_EVENT_PROFILE_PRINT_MINIMUM_PROPORTION = 0.01;

void IO_Event_Profile_print(FILE *restrict stream, struct IO_Event_Profile *profile) {
	struct timespec total_duration = {};
	IO_Event_Time_elapsed(&profile->start_time, &profile->stop_time, &total_duration);
	
	size_t skipped = 0;
	
	for (size_t i = 0; i < profile->events.limit; i += 1) {
		struct IO_Event_Profile_Call *event = profile->events.base[i];
		
		struct timespec duration = {};
		
		IO_Event_Time_elapsed(&event->enter_time, &event->exit_time, &duration);
		
		// Skip events that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILE_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		for (size_t i = 0; i < event->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		const char *name = rb_id2name(event->id);
		fprintf(stream, "\t%s:%d in '%s#%s' (" IO_EVENT_TIME_PRINTF_TIMESPEC "s)\n", event->path, event->line, RSTRING_PTR(rb_inspect(event->klass)), name, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
	}
	
	if (skipped > 0) {
		fprintf(stream, "Skipped %zu events that were too short to be meaningful.\n", skipped);
	}
}
