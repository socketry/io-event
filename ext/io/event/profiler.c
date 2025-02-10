// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "profiler.h"
#include "time.h"
#include "fiber.h"

#include <ruby/debug.h>

#include <stdio.h>

static const int DEBUG = 0;

VALUE IO_Event_Profiler = Qnil;

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
	}
}

static void IO_Event_Profiler_mark(void *ptr) {
	struct IO_Event_Profiler *profiler = (struct IO_Event_Profiler*)ptr;
	
	rb_gc_mark_movable(profiler->self);
	
	// If `klass` is stored as a VALUE in calls, we need to mark them here:
	for (size_t i = 0; i < profiler->calls.limit; i += 1) {
		struct IO_Event_Profiler_Call *call = profiler->calls.base[i];
		rb_gc_mark_movable(call->klass);
	}
}

static void IO_Event_Profiler_compact(void *ptr) {
	struct IO_Event_Profiler *profiler = (struct IO_Event_Profiler*)ptr;
	profiler->self = rb_gc_location(profiler->self);
	
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

VALUE IO_Event_Profiler_allocate(VALUE klass) {
	struct IO_Event_Profiler *profiler = ALLOC(struct IO_Event_Profiler);
	
	profiler->running = 0;
	
	profiler->calls.element_initialize = (void (*)(void*))IO_Event_Profiler_Call_initialize;
	profiler->calls.element_free = (void (*)(void*))IO_Event_Profiler_Call_free;
	
	IO_Event_Array_initialize(&profiler->calls, 0, sizeof(struct IO_Event_Profiler_Call));
	
	VALUE self = TypedData_Wrap_Struct(klass, &IO_Event_Profiler_Type, profiler);
	RB_OBJ_WRITE(self, &profiler->self, self);
	
	return self;
}

VALUE IO_Event_Profiler_new(float log_threshold, int track_calls) {
	VALUE profiler = IO_Event_Profiler_allocate(IO_Event_Profiler);
	
	struct IO_Event_Profiler *profiler_data = IO_Event_Profiler_get(profiler);
	profiler_data->log_threshold = log_threshold;
	profiler_data->track_calls = track_calls;
	
	return profiler;
}

struct IO_Event_Profiler *IO_Event_Profiler_get(VALUE self) {
	struct IO_Event_Profiler *profiler;
	TypedData_Get_Struct(self, struct IO_Event_Profiler, &IO_Event_Profiler_Type, profiler);
	return profiler;
}

int event_flag_call_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_CALL | RUBY_EVENT_C_CALL);
}

int event_flag_return_p(rb_event_flag_t event_flags) {
	return event_flags & (RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN);
}

const char *event_flag_name(rb_event_flag_t event_flag) {
	switch (event_flag) {
		case RUBY_EVENT_CALL: return "call";
		case RUBY_EVENT_C_CALL: return "c-call";
		case RUBY_EVENT_RETURN: return "return";
		case RUBY_EVENT_C_RETURN: return "c-return";
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

static void profiler_event_callback(rb_event_flag_t event_flag, VALUE data, VALUE self, ID id, VALUE klass) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(data);
	
	if (event_flag_call_p(event_flag)) {
		struct IO_Event_Profiler_Call *call = profiler_event_record_call(profiler, event_flag, id, klass);
		IO_Event_Time_current(&call->enter_time);
	} else if (event_flag_return_p(event_flag)) {
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

void IO_Event_Profiler_start(struct IO_Event_Profiler *profiler) {
	profiler->running = 1;
	
	IO_Event_Time_current(&profiler->start_time);
	
	profiler->nesting = 0;
	profiler->current = NULL;
	
	// Since fibers are currently limited to a single thread, we use this in the hope that it's a little more efficient:
	if (profiler->track_calls) {
		VALUE thread = rb_thread_current();
		rb_thread_add_event_hook(thread, profiler_event_callback, RUBY_EVENT_CALL | RUBY_EVENT_C_CALL | RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN, profiler->self);
	}
}

static inline float IO_Event_Profiler_duration(struct IO_Event_Profiler *profiler) {
	struct timespec duration;
	
	IO_Event_Time_elapsed(&profiler->start_time, &profiler->stop_time, &duration);
	
	return IO_Event_Time_duration(&duration);
}

void IO_Event_Profiler_print(struct IO_Event_Profiler *profiler, FILE *restrict stream);

void IO_Event_Profiler_stop(struct IO_Event_Profiler *profiler) {
	profiler->running = 0;
	
	if (profiler->track_calls) {
		VALUE thread = rb_thread_current();
		rb_thread_remove_event_hook_with_data(thread, profiler_event_callback, profiler->self);
	}
	
	IO_Event_Time_current(&profiler->stop_time);
	float duration = IO_Event_Profiler_duration(profiler);
	
	if (duration > profiler->log_threshold) {
		IO_Event_Profiler_print(profiler, stderr);
	}
}

void IO_Event_Profiler_restart(struct IO_Event_Profiler *profiler) {
	IO_Event_Profiler_stop(profiler);
	IO_Event_Array_truncate(&profiler->calls, 0);
	IO_Event_Profiler_start(profiler);
}

VALUE IO_Event_Profiler_fiber_transfer(VALUE self, VALUE fiber, int argc, VALUE *argv) {
	struct IO_Event_Profiler *profiler = IO_Event_Profiler_get(self);
	int running = 0;
	
	// If we are running when we enter, that means we are currently profiling, and we need to restart the profiler when we exit:
	if (!profiler->running) {
		IO_Event_Profiler_start(profiler);
	} else {
		running = profiler->running;
		
		// We are switching to a different fiber, so consider the current profile complete:
		IO_Event_Profiler_restart(profiler);
	}
	
	if (DEBUG) fprintf(stderr, "Transferring to fiber %p\n", (void*)fiber);
	VALUE result = IO_Event_Fiber_transfer(fiber, argc, argv);
	
	if (running) {
		// If the profiler was running, we need to restart it:
		IO_Event_Profiler_restart(profiler);
	} else {
		// Otherwise, we need to stop it:
		IO_Event_Profiler_stop(profiler);
	}
	
	return result;
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
		
		const char *prefix = "";
		
		// Skip calls that are too short to be meaningful:
		if (IO_Event_Time_proportion(&duration, &total_duration) < IO_EVENT_PROFILER_PRINT_MINIMUM_PROPORTION) {
			skipped += 1;
			continue;
			//prefix = "*";
		}
		
		for (size_t i = 0; i < call->nesting; i += 1) {
			fputc('\t', stream);
		}
		
		VALUE class_inspect = rb_inspect(call->klass);
		const char *name = rb_id2name(call->id);
		
		fprintf(stream, "%s%s:%d in %s '%s#%s' (" IO_EVENT_TIME_PRINTF_TIMESPEC "s)\n", prefix, call->path, call->line, event_flag_name(call->event_flag), RSTRING_PTR(class_inspect), name, IO_EVENT_TIME_PRINTF_TIMESPEC_ARGUMENTS(duration));
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

void Init_IO_Event_Profiler(VALUE IO_Event) {
	IO_Event_Profiler = rb_define_class_under(IO_Event, "Profiler", rb_cObject);
	rb_define_alloc_func(IO_Event_Profiler, IO_Event_Profiler_allocate);
}
