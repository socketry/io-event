// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profile.h"
#include "time.h"

#include <ruby/debug.h>

#include <stdio.h>

VALUE IO_Event_Profile = Qnil;

void IO_Event_Profile_Call_initialize(struct IO_Event_Profile_Call *call) {
	call->enter_time.tv_sec = 0;
	call->enter_time.tv_nsec = 0;
	call->exit_time.tv_sec = 0;
	call->exit_time.tv_nsec = 0;
	
	call->nesting = 0;
	
	call->event_flag = 0;
	call->id = 0;
	
	call->path = NULL;
	call->line = 0;
}

void IO_Event_Profile_Call_free(struct IO_Event_Profile_Call *call) {
	if (call->path) {
		free((void*)call->path);
	}
}

static void IO_Event_Profile_mark(void *ptr) {
	struct IO_Event_Profile *profile = (struct IO_Event_Profile*)ptr;
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	for (size_t i = 0; i < profile->calls.limit; i += 1) {
		struct IO_Event_Profile_Call *call = profile->calls.base[i];
		rb_gc_mark(call->klass);
	}
}

static void IO_Event_Profile_free(void *ptr) {
	struct IO_Event_Profile *profile = (struct IO_Event_Profile*)ptr;
	
	IO_Event_Array_free(&profile->calls);
	
	free(profile);
}

const rb_data_type_t IO_Event_Profile_Type = {
	.wrap_struct_name = "IO_Event_Profile",
	.function = {
		.dmark = IO_Event_Profile_mark,
		.dfree = IO_Event_Profile_free,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE IO_Event_Profile_allocate(VALUE klass) {
	struct IO_Event_Profile *profile = ALLOC(struct IO_Event_Profile);
	
	profile->calls.element_initialize = (void (*)(void*))IO_Event_Profile_Call_initialize;
	profile->calls.element_free = (void (*)(void*))IO_Event_Profile_Call_free;
	
	IO_Event_Array_initialize(&profile->calls, 0, sizeof(struct IO_Event_Profile_Call));
	
	return TypedData_Wrap_Struct(klass, &IO_Event_Profile_Type, profile);
}

struct IO_Event_Profile *IO_Event_Profile_get(VALUE self) {
	struct IO_Event_Profile *profile;
	TypedData_Get_Struct(self, struct IO_Event_Profile, &IO_Event_Profile_Type, profile);
	return profile;
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN);
}

static void profile_event_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(data);
	
	if (event_flag_call_p(event_flag)) {
		struct IO_Event_Profile_Call *call = IO_Event_Array_push(&profile->calls);
		IO_Event_Time_current(&call->enter_time);
	
		call->event_flag = event_flag;
		
		call->parent = profile->current;
		profile->current = call;
		
		call->nesting = profile->nesting;
		profile->nesting += 1;
		
		if (id) {
			call->id = id;
			call->klass = klass;
		} else {
			rb_frame_method_id_and_class(&call->id, &call->klass);
		}
		
		const char *path = rb_sourcefile();
		if (path) {
			call->path = strdup(path);
		}
		call->line = rb_sourceline();
	} else if (event_flag_return_p(event_flag)) {
		struct IO_Event_Profile_Call *call = profile->current;
		
		// Bad call sequence?
		if (call == NULL) return;
		
		IO_Event_Time_current(&call->exit_time);
		
		profile->current = call->parent;
		profile->nesting -= 1;
	}
}

void IO_Event_Profile_start(VALUE self, int track_calls) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(self);
	
	IO_Event_Time_current(&profile->start_time);
	profile->nesting = 0;
	profile->current = NULL;
	
	profile->track_calls = track_calls;
	
	// Since fibers are currently limited to a single thread, we use this in the hope that it's a little more efficient:
	if (profile->track_calls) {
		VALUE thread = rb_thread_current();
		rb_thread_add_event_hook(thread, profile_event_callback, RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN, self);
	}
}

void IO_Event_Profile_stop(VALUE self) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(self);
	
	IO_Event_Time_current(&profile->stop_time);
	
	if (profile->track_calls) {
		VALUE thread = rb_thread_current();
		rb_thread_remove_event_hook_with_data(thread, profile_event_callback, self);
	}
}

static const float IO_EVENT_PROFILE_PRINT_MINIMUM_PROPORTION = 0.01;

void IO_Event_Profile_print_tty(VALUE self, FILE *restrict stream) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(self);
	
	struct timespec total_duration = {};
	IO_Event_Time_elapsed(&profile->start_time, &profile->stop_time, &total_duration);
	
	fprintf(stderr, "Fiber stalled for %.3f seconds\n", total_duration);
	
	size_t skipped = 0;
	
	for (size_t i = 0; i < profile->calls.limit; i += 1) {
		struct IO_Event_Profile_Call *call = profile->calls.base[i];
		struct timespec duration = {};
		IO_Event_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILE_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		for (size_t i = 0; i < call->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "\t%s:%d in '%s#%s' (" IO_EVENT_TIME_PRINTF_TIMESPEC "s)\n", call->path, call->line, RSTRING_PTR(class_inspect), name, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
	}
	
	if (skipped > 0) {
		fprintf(stream, "Skipped %zu calls that were too short to be meaningful.\n", skipped);
	}
}

void IO_Event_Profile_print_json(VALUE self, FILE *restrict stream) {
	struct IO_Event_Profile *profile = IO_Event_Profile_get(self);
	
	struct timespec total_duration = {};
	IO_Event_Time_elapsed(&profile->start_time, &profile->stop_time, &total_duration);
	
	fputc('{', stream);
	
	fprintf(stream, "\"duration\":" IO_EVENT_TIME_PRINTF_TIMESPEC, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(total_duration));
	
	size_t skipped = 0;
	
	fprintf(stream, ",\"calls\":[");
	int first = 1;
	
	for (size_t i = 0; i < profile->calls.limit; i += 1) {
		struct IO_Event_Profile_Call *call = profile->calls.base[i];
		struct timespec duration = {};
		IO_Event_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILE_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s{\"path\":\"%s\",\"line\":%d,\"class\":\"%s\",\"method\":\"%s\",\"duration\":" IO_EVENT_TIME_PRINTF_TIMESPEC ",\"nesting\":%zu}", first ? "" : ",", call->path, call->line, RSTRING_PTR(class_inspect), name, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration), call->nesting);
		
		first = 0;
	}
	
	fprintf(stream, "]");
	
	if (skipped > 0) {
		fprintf(stream, ",\"skipped\":%zu", skipped);
	}
	
	fprintf(stream, "}\n");
}

void IO_Event_Profile_print(VALUE self, FILE *restrict stream) {
	if (isatty(fileno(stream))) {
		IO_Event_Profile_print_tty(self, stream);
	} else {
		IO_Event_Profile_print_json(self, stream);
	}
}

void Init_IO_Event_Profile(VALUE IO_Event) {
	IO_Event_Profile = rb_define_class_under(IO_Event, "Profile", rb_cObject);
	rb_define_alloc_func(IO_Event_Profile, IO_Event_Profile_allocate);
}
