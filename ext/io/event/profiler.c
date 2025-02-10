// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"

#include "time.h"
#include "fiber.h"
#include "array.h"

#include <ruby/debug.h>
#include <stdio.h>

VALUE IO_Event_Profiler = Qnil;

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
	// Configuration:
	float log_threshold;
	int track_calls;
	
	// Whether or not the profiler is currently running:
	int running;
	
	// Whether or not to capture call data:
	int capture;
	
	size_t stalls;
	
	// From this point on, the state of any profile in progress:
	struct timespec start_time;
	struct timespec stop_time;
	
	// The depth of the call stack:
	size_t nesting;
	
	// The current call frame:
	struct IO_Event_Profiler_Call *current;
	
	struct IO_Event_Array calls;
};

void IO_Event_Profiler_reset(struct IO_Event_Profiler *profiler) {
	profiler->nesting = 0;
	profiler->current = NULL;
	IO_Event_Array_truncate(&profiler->calls, 0);
}

void IO_Event_Profiler_Call_initialize(struct IO_Event_Profiler_Call *call) {
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

void IO_Event_Profiler_Call_free(struct IO_Event_Profiler_Call *call) {
	if (call->path) {
		free((void*)call->path);
		call->path = NULL;
	}
}

static void IO_Event_Profiler_mark(void *ptr) {
	struct IO_Event_Profiler *profiler = (struct IO_Event_Profiler*)ptr;
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct IO_Event_Profiler_Call *call = profiler->calls.base[i];
		rb_gc_mark_movable(call->klass);
	}
}

static void IO_Event_Profiler_compact(void *ptr) {
	struct IO_Event_Profiler *profiler = (struct IO_Event_Profiler*)ptr;
	
	// If `klass` is stored as a VALUE in calls, we need to update their locations here:
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
			struct IO_Event_Profiler_Call *call = profiler->calls.base[i];
			call->klass = rb_gc_location(call->klass);
	}
}

static void IO_Event_Profiler_free(void *ptr) {
	struct IO_Event_Profiler *profiler = (struct IO_Event_Profiler*)ptr;
	
	IO_Event_Array_free(&profiler->calls);
	
	free(profiler);
}

static size_t IO_Event_Profiler_memsize(const void *ptr) {
	const struct IO_Event_Profiler *profiler = (const struct IO_Event_Profiler*)ptr;
	return sizeof(*profiler) + IO_Event_Array_memory_size(&profiler->calls);
}

const rb_data_type_t IO_Event_Profiler_Type = {
	.wrap_struct_name = "IO::Event::Profiler",
	.function = {
		.dmark = IO_Event_Profiler_mark,
		.dcompact = IO_Event_Profiler_compact,
		.dfree = IO_Event_Profiler_free,
		.dsize = IO_Event_Profiler_memsize,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

struct IO_Event_Profiler *IO_Event_Profiler_get(VALUE self) {
	struct IO_Event_Profiler *profiler;
	TypedData_Get_Struct(self, struct IO_Event_Profiler, &IO_Event_Profiler_Type, profiler);
	return profiler;
}

VALUE IO_Event_Profiler_allocate(VALUE klass) {
	struct IO_Event_Profiler *profiler = ALLOC(struct IO_Event_Profiler);
	
	// Initialize the profiler state:
	profiler->running = 0;
	profiler->capture = 0;
	profiler->stalls = 0;
	profiler->nesting = 0;
	profiler->current = NULL;
	
	profiler->calls.element_initialize = (void (*)(void*))IO_Event_Profiler_Call_initialize;
	profiler->calls.element_free = (void (*)(void*))IO_Event_Profiler_Call_free;
	IO_Event_Array_initialize(&profiler->calls, 0, sizeof(struct IO_Event_Profiler_Call));
	
	return TypedData_Wrap_Struct(klass, &IO_Event_Profiler_Type, profiler);
}

int IO_Event_Profiler_p(void) {
	const char *enabled = getenv("IO_EVENT_PROFILER");
	
	if (enabled && strcmp(enabled, "true") == 0) {
		return 1;
	}
	
	return 0;
}

float IO_Event_Profiler_default_log_threshold(void) {
	const char *log_threshold = getenv("IO_EVENT_PROFILER_LOG_THRESHOLD");
	
	if (log_threshold) {
		return strtof(log_threshold, NULL);
	} else {
		return 0.01;
	}
}

int IO_Event_Profiler_default_track_calls(void) {
	const char *track_calls = getenv("IO_EVENT_PROFILER_TRACK_CALLS");
	
	if (track_calls && strcmp(track_calls, "false") == 0) {
		return 0;
	} else {
		return 1;
	}
}

VALUE IO_Event_Profiler_initialize(int argc, VALUE *argv, VALUE self) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(self);
	VALUE log_threshold, track_calls;
	
	rb_scan_args(argc, argv, "02", &log_threshold, &track_calls);
	
	if (RB_NIL_P(log_threshold)) {
		profiler->log_threshold = IO_Event_Profiler_default_log_threshold();
	} else {
		profiler->log_threshold = NUM2DBL(log_threshold);
	}
	
	if (RB_NIL_P(track_calls)) {
		profiler->track_calls = IO_Event_Profiler_default_track_calls();
	} else {
		profiler->track_calls = RB_TEST(track_calls);
	}
	
	return self;
}

VALUE IO_Event_Profiler_default(VALUE klass) {
	if (!IO_Event_Profiler_p()) {
		return Qnil;
	}
	
	VALUE profiler = IO_Event_Profiler_allocate(klass);
	
	struct IO_Event_Profiler *profiler_data = IO_Event_Profiler_get(profiler);
	profiler_data->log_threshold = IO_Event_Profiler_default_log_threshold();
	profiler_data->track_calls = IO_Event_Profiler_default_track_calls();
	
	return profiler;
}

VALUE IO_Event_Profiler_new(float log_threshold, int track_calls) {
	VALUE profiler = IO_Event_Profiler_allocate(IO_Event_Profiler);
	
	struct IO_Event_Profiler *profiler_data = IO_Event_Profiler_get(profiler);
	profiler_data->log_threshold = log_threshold;
	profiler_data->track_calls = track_calls;
	
	return profiler;
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_B_CALL);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN);
}

const char *event_flag_name(rb_event_flag_t event_flag) {
	switch (event_flag) {
		case RUBY_EVENT_CALL: return "call";
		case RUBY_EVENT_C_CALL: return "c-call";
		case RUBY_EVENT_B_CALL: return "b-call";
		case RUBY_EVENT_RETURN: return "return";
		case RUBY_EVENT_C_RETURN: return "c-return";
		case RUBY_EVENT_B_RETURN: return "b-return";
		default: return "unknown";
	}
}

static struct IO_Event_Profiler_Call* profiler_event_record_call(struct IO_Event_Profiler *profiler, rb_event_flag_t event_flag, ID id, VALUE klass) {
	struct IO_Event_Profiler_Call *call = IO_Event_Array_push(&profiler->calls);
	
	call->event_flag = event_flag;

	call->parent = profiler->current;
	profiler->current = call;

	call->nesting = profiler->nesting;
	profiler->nesting += 1;

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
	
	return call;
}

void IO_Event_Profiler_fiber_switch(struct IO_Event_Profiler *profiler);

static void IO_Event_Profiler_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(data);
	
	if (event_flag & RUBY_EVENT_FIBER_SWITCH) {
		IO_Event_Profiler_fiber_switch(profiler);
		return;
	}
	
	// We don't want to capture data if we're not running:
	if (!profiler->capture) return;
	
	if (event_flag_call_p(event_flag)) {
		struct IO_Event_Profiler_Call *call = profiler_event_record_call(profiler, event_flag, id, klass);
		IO_Event_Time_current(&call->enter_time);
	}
	
	else if (event_flag_return_p(event_flag)) {
		struct IO_Event_Profiler_Call *call = profiler->current;
		
		// We may encounter returns without a preceeding call. This isn't an error, but we should pretend like the call started at the beginning of the profiling session:
		if (call == NULL) {
			struct IO_Event_Profiler_Call *last_call = IO_Event_Array_last(&profiler->calls);
			call = profiler_event_record_call(profiler, event_flag, id, klass);
			
			if (last_call) {
				call->enter_time = last_call->enter_time;
			} else {
				call->enter_time = profiler->start_time;
			}
		}
		
		IO_Event_Time_current(&call->exit_time);
		
		profiler->current = call->parent;
		
		// We may encounter returns without a preceeding call.
		if (profiler->nesting > 0)
			profiler->nesting -= 1;
	}
}

VALUE IO_Event_Profiler_start(VALUE self) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(self);
	
	if (profiler->running) return Qfalse;
	
	profiler->running = 1;
	
	IO_Event_Profiler_reset(profiler);
	IO_Event_Time_current(&profiler->start_time);
	
	rb_event_flag_t event_flags = RUBY_EVENT_FIBER_SWITCH;
	
	if (profiler->track_calls) {
		event_flags |= RUBY_EVENT_CALL | RUBY_EVENT_RETURN;
		event_flags |= RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN;
		// event_flags |= RUBY_EVENT_B_CALL | RUBY_EVENT_B_RETURN;
	}
	
	VALUE thread = rb_thread_current();
	rb_thread_add_event_hook(thread, IO_Event_Profiler_callback, event_flags, self);
	
	return self;
}

VALUE IO_Event_Profiler_stop(VALUE self) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(self);
	
	if (!profiler->running) return Qfalse;
	
	profiler->running = 0;
	
	VALUE thread = rb_thread_current();
	rb_thread_remove_event_hook_with_data(thread, IO_Event_Profiler_callback, self);
	
	IO_Event_Time_current(&profiler->stop_time);
	IO_Event_Profiler_reset(profiler);
	
	return self;
}

static inline float IO_Event_Profiler_duration(struct IO_Event_Profiler *profiler) {
	struct timespec duration;
	
	IO_Event_Time_current(&profiler->stop_time);
	IO_Event_Time_elapsed(&profiler->start_time, &profiler->stop_time, &duration);
	
	return IO_Event_Time_duration(&duration);
}

void IO_Event_Profiler_print(struct IO_Event_Profiler *profiler, FILE *restrict stream);

void IO_Event_Profiler_finish(struct IO_Event_Profiler *profiler) {
	profiler->capture = 0;
	
	struct IO_Event_Profiler_Call *current = profiler->current;
	while (current) {
		IO_Event_Time_current(&current->exit_time);
		
		current = current->parent;
	}
}

void IO_Event_Profiler_fiber_switch(struct IO_Event_Profiler *profiler)
{
	float duration = IO_Event_Profiler_duration(profiler);
	
	if (profiler->capture) {
		IO_Event_Profiler_finish(profiler);
		
		if (duration > profiler->log_threshold) {
			profiler->stalls += 1;
			IO_Event_Profiler_print(profiler, stderr);
		}
	}
	
	IO_Event_Profiler_reset(profiler);
	
	if (!IO_Event_Fiber_blocking(IO_Event_Fiber_current())) {
		// Reset the start time:
		IO_Event_Time_current(&profiler->start_time);
		
		profiler->capture = 1;
	}
}

static const float IO_EVENT_PROFILER_PRINT_MINIMUM_PROPORTION = 0.01;

void IO_Event_Profiler_print_tty(struct IO_Event_Profiler *profiler, FILE *restrict stream) {
	struct timespec total_duration = {};
	IO_Event_Time_elapsed(&profiler->start_time, &profiler->stop_time, &total_duration);
	
	fprintf(stderr, "Fiber stalled for %.3f seconds\n", IO_Event_Time_duration(&total_duration));
	
	size_t skipped = 0;
	
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct IO_Event_Profiler_Call *call = profiler->calls.base[i];
		struct timespec duration = {};
		IO_Event_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILER_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
		}
		
		for (size_t i = 0; i < call->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s:%d in %s '%s#%s' (" IO_EVENT_TIME_PRINTF_TIMESPEC "s)\n", call->path, call->line, event_flag_name(call->event_flag), RSTRING_PTR(class_inspect), name, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
	}
	
	if (skipped > 0) {
		fprintf(stream, "Skipped %zu calls that were too short to be meaningful.\n", skipped);
	}
}

void IO_Event_Profiler_print_json(struct IO_Event_Profiler *profiler, FILE *restrict stream) {
	struct timespec total_duration = {};
	IO_Event_Time_elapsed(&profiler->start_time, &profiler->stop_time, &total_duration);
	
	fputc('{', stream);
	
	fprintf(stream, "\"duration\":" IO_EVENT_TIME_PRINTF_TIMESPEC, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(total_duration));
	
	size_t skipped = 0;
	
	fprintf(stream, ",\"calls\":[");
	int first = 1;
	
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct IO_Event_Profiler_Call *call = profiler->calls.base[i];
		struct timespec duration = {};
		IO_Event_Time_elapsed(&call->enter_time, &call->exit_time, &duration);
		
		// Skip calls that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILER_PRINT_MINIMUM_PROPORTION) {
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

void IO_Event_Profiler_print(struct IO_Event_Profiler *profiler, FILE *restrict stream) {
	if (isatty(fileno(stream))) {
		IO_Event_Profiler_print_tty(profiler, stream);
	} else {
		IO_Event_Profiler_print_json(profiler, stream);
	}
}

VALUE IO_Event_Profiler_stalls(VALUE self) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(self);
	
	return SIZET2NUM(profiler->stalls);
}

void Init_IO_Event_Profiler(VALUE IO_Event) {
	IO_Event_Profiler = rb_define_class_under(IO_Event, "Profiler", rb_cObject);
	rb_define_alloc_func(IO_Event_Profiler, IO_Event_Profiler_allocate);
	
	rb_define_singleton_method(IO_Event_Profiler, "default", IO_Event_Profiler_default, 0);
	
	rb_define_method(IO_Event_Profiler, "initialize", IO_Event_Profiler_initialize, -1);
	
	rb_define_method(IO_Event_Profiler, "start", IO_Event_Profiler_start, 0);
	rb_define_method(IO_Event_Profiler, "stop", IO_Event_Profiler_stop, 0);
	
	rb_define_method(IO_Event_Profiler, "stalls", IO_Event_Profiler_stalls, 0);
}
