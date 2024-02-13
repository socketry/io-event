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

#pragma once

#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>

#ifdef HAVE_RUBY_IO_BUFFER_H
#include <ruby/io/buffer.h>
#include <ruby/fiber/scheduler.h>
#endif

#ifndef RUBY_FIBER_SCHEDULER_VERSION
#define RUBY_FIBER_SCHEDULER_VERSION 1
#endif

#include <time.h>
#ifdef HAVE_RB_PROCESS_STATUS_WAIT
  #include <sys/wait.h>
#endif

enum IO_Event {
	IO_EVENT_READABLE = 1,
	IO_EVENT_PRIORITY = 2,
	IO_EVENT_WRITABLE = 4,
	IO_EVENT_ERROR = 8,
	IO_EVENT_HANGUP = 16,
	
	// Used by kqueue to differentiate between process exit and file descriptor events:
	IO_EVENT_EXIT = 32,
};

void Init_IO_Event_Selector(VALUE IO_Event_Selector);

static inline int IO_Event_try_again(int error) {
	return error == EAGAIN || error == EWOULDBLOCK;
}

VALUE IO_Event_Selector_fiber_transfer(VALUE fiber, int argc, VALUE *argv);

#ifdef HAVE__RB_FIBER_RAISE
#define IO_Event_Selector_fiber_raise(fiber, argc, argv) rb_fiber_raise(fiber, argc, argv)
#else
VALUE IO_Event_Selector_fiber_raise(VALUE fiber, int argc, VALUE *argv);
#endif

#ifdef HAVE_RB_IO_DESCRIPTOR
#define IO_Event_Selector_io_descriptor(io) rb_io_descriptor(io)
#else
int IO_Event_Selector_io_descriptor(VALUE io);
#endif

// Reap a process without hanging.
#ifdef HAVE_RB_PROCESS_STATUS_WAIT
#define IO_Event_Selector_process_status_wait(pid, flags) rb_process_status_wait(pid, flags | WNOHANG)
#else
VALUE IO_Event_Selector_process_status_wait(rb_pid_t pid, int flags);
#endif

int IO_Event_Selector_nonblock_set(int file_descriptor);
void IO_Event_Selector_nonblock_restore(int file_descriptor, int flags);

enum IO_Event_Selector_Queue_Flags {
	IO_EVENT_SELECTOR_QUEUE_FIBER = 1,
	IO_EVENT_SELECTOR_QUEUE_INTERNAL = 2,
};

struct IO_Event_Selector_Queue {
	struct IO_Event_Selector_Queue *head;
	struct IO_Event_Selector_Queue *tail;
	
	enum IO_Event_Selector_Queue_Flags flags;
	
	VALUE fiber;
};

struct IO_Event_Selector {
	VALUE loop;
	
	struct IO_Event_Selector_Queue *free;
	
	// Append to waiting.
	struct IO_Event_Selector_Queue *waiting;
	// Process from ready.
	struct IO_Event_Selector_Queue *ready;
};

static inline
void IO_Event_Selector_initialize(struct IO_Event_Selector *backend, VALUE loop) {
	backend->loop = loop;
	backend->waiting = NULL;
	backend->ready = NULL;
}

static inline
void IO_Event_Selector_mark(struct IO_Event_Selector *backend) {
	rb_gc_mark_movable(backend->loop);
	
	struct IO_Event_Selector_Queue *ready = backend->ready;
	while (ready) {
		rb_gc_mark_movable(ready->fiber);
		ready = ready->head;
	}
}

static inline
void IO_Event_Selector_compact(struct IO_Event_Selector *backend) {
	backend->loop = rb_gc_location(backend->loop);
	
	struct IO_Event_Selector_Queue *ready = backend->ready;
	while (ready) {
		ready->fiber = rb_gc_location(ready->fiber);
		ready = ready->head;
	}
}

VALUE IO_Event_Selector_resume(struct IO_Event_Selector *backend, int argc, VALUE *argv);
VALUE IO_Event_Selector_raise(struct IO_Event_Selector *backend, int argc, VALUE *argv);

static inline
VALUE IO_Event_Selector_yield(struct IO_Event_Selector *backend)
{
	return IO_Event_Selector_resume(backend, 1, &backend->loop);
}

void IO_Event_Selector_queue_push(struct IO_Event_Selector *backend, VALUE fiber);
int IO_Event_Selector_queue_flush(struct IO_Event_Selector *backend);

void IO_Event_Selector_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration);
void IO_Event_Selector_current_time(struct timespec *time);

#define PRINTF_TIMESPEC "%lld.%.9ld"
#define PRINTF_TIMESPEC_ARGS(ts) (long long)((ts).tv_sec), (ts).tv_nsec
