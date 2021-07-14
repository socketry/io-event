// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>

#ifdef HAVE_RUBY_IO_BUFFER_H
#include <ruby/io/buffer.h>
#endif

#include <time.h>

enum Event {
	EVENT_READABLE = 1,
	EVENT_PRIORITY = 2,
	EVENT_WRITABLE = 4,
	EVENT_ERROR = 8,
	EVENT_HANGUP = 16
};

void Init_Event_Selector();

#ifdef HAVE__RB_FIBER_TRANSFER
#define Event_Selector_fiber_transfer(fiber, argc, argv) rb_fiber_transfer(fiber, argc, argv)
#else
VALUE Event_Selector_fiber_transfer(VALUE fiber, int argc, VALUE *argv);
#endif

#ifdef HAVE__RB_FIBER_RAISE
#define Event_Selector_fiber_raise(fiber, argc, argv) rb_fiber_raise(fiber, argc, argv)
#else
VALUE Event_Selector_fiber_raise(VALUE fiber, int argc, VALUE *argv);
#endif

#ifdef HAVE_RB_IO_DESCRIPTOR
#define Event_Selector_io_descriptor(io) rb_io_descriptor(io)
#else
int Event_Selector_io_descriptor(VALUE io);
#endif

#ifdef HAVE_RB_PROCESS_STATUS_WAIT
#define Event_Selector_process_status_wait(pid) rb_process_status_wait(pid)
#else
VALUE Event_Selector_process_status_wait(rb_pid_t pid);
#endif

int Event_Selector_nonblock_set(int file_descriptor);
void Event_Selector_nonblock_restore(int file_descriptor, int flags);

enum Event_Selector_Queue_Flags {
	EVENT_SELECTOR_QUEUE_FIBER = 1,
	EVENT_SELECTOR_QUEUE_INTERNAL = 2,
};

struct Event_Selector_Queue {
	struct Event_Selector_Queue *behind;
	struct Event_Selector_Queue *infront;
	
	enum Event_Selector_Queue_Flags flags;
	
	VALUE fiber;
};

struct Event_Selector {
	VALUE loop;
	
	struct Event_Selector_Queue *free;
	
	// Append to waiting.
	struct Event_Selector_Queue *waiting;
	// Process from ready.
	struct Event_Selector_Queue *ready;
};

static inline
void Event_Selector_initialize(struct Event_Selector *backend, VALUE loop) {
	backend->loop = loop;
	backend->waiting = NULL;
	backend->ready = NULL;
}

static inline
void Event_Selector_mark(struct Event_Selector *backend) {
	rb_gc_mark(backend->loop);
	
	struct Event_Selector_Queue *ready = backend->ready;
	while (ready) {
		rb_gc_mark(ready->fiber);
		ready = ready->behind;
	}
}

VALUE Event_Selector_wait_and_transfer(struct Event_Selector *backend, int argc, VALUE *argv);
VALUE Event_Selector_wait_and_raise(struct Event_Selector *backend, int argc, VALUE *argv);

static inline
VALUE Event_Selector_yield(struct Event_Selector *backend)
{
	return Event_Selector_wait_and_transfer(backend, 1, &backend->loop);
}

void Event_Selector_queue_push(struct Event_Selector *backend, VALUE fiber);
int Event_Selector_queue_flush(struct Event_Selector *backend);

void Event_Selector_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration);
void Event_Selector_current_time(struct timespec *time);

#define PRINTF_TIMESPEC "%lld.%.9ld"
#define PRINTF_TIMESPEC_ARGS(ts) (long long)((ts).tv_sec), (ts).tv_nsec
