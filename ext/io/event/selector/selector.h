// Released under the MIT License.
// Copyright, 2021-2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/io.h>

#include "../time.h"

#ifdef HAVE_RUBY_IO_BUFFER_H
#include <ruby/io/buffer.h>
#include <ruby/fiber/scheduler.h>
#endif

#ifndef RUBY_FIBER_SCHEDULER_VERSION
#define RUBY_FIBER_SCHEDULER_VERSION 1
#endif

#ifdef HAVE_SYS_WAIT_H
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

// Specifically for transferring control to a user fiber.
VALUE IO_Event_Selector_fiber_transfer_user(VALUE fiber, int argc, VALUE *argv);

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
	VALUE self;
	VALUE loop;
	
	// Append to waiting (front/head of queue).
	struct IO_Event_Selector_Queue *waiting;
	// Process from ready (back/tail of queue).
	struct IO_Event_Selector_Queue *ready;
};

static inline
void IO_Event_Selector_initialize(struct IO_Event_Selector *backend, VALUE self, VALUE loop) {
	RB_OBJ_WRITE(self, &backend->self, self);
	RB_OBJ_WRITE(self, &backend->loop, loop);
	backend->waiting = NULL;
	backend->ready = NULL;
}

static inline
void IO_Event_Selector_mark(struct IO_Event_Selector *backend) {
	rb_gc_mark_movable(backend->self);
	rb_gc_mark_movable(backend->loop);
	
	// Walk backwards through the ready queue:
	struct IO_Event_Selector_Queue *ready = backend->ready;
	while (ready) {
		rb_gc_mark_movable(ready->fiber);
		ready = ready->head;
	}
}

static inline
void IO_Event_Selector_compact(struct IO_Event_Selector *backend) {
	backend->self = rb_gc_location(backend->self);
	backend->loop = rb_gc_location(backend->loop);
	
	struct IO_Event_Selector_Queue *ready = backend->ready;
	while (ready) {
		ready->fiber = rb_gc_location(ready->fiber);
		ready = ready->head;
	}
}

// Transfer from a user fiber back to the event loop.
static inline
VALUE IO_Event_Selector_backend_yield(struct IO_Event_Selector *backend)
{
	RUBY_ASSERT(backend->loop != rb_fiber_current());
	
	return IO_Event_Selector_fiber_transfer(backend->loop, 0, NULL);
}

VALUE IO_Event_Selector_resume(struct IO_Event_Selector *backend, int argc, VALUE *argv);
VALUE IO_Event_Selector_raise(struct IO_Event_Selector *backend, int argc, VALUE *argv);

static inline
VALUE IO_Event_Selector_yield(struct IO_Event_Selector *backend)
{
	return IO_Event_Selector_resume(backend, 1, &backend->loop);
}

// Append to the ready queue.
void IO_Event_Selector_ready_push(struct IO_Event_Selector *backend, VALUE fiber);

// Flush the ready queue by transferring control one at a time.
int IO_Event_Selector_ready_flush(struct IO_Event_Selector *backend);
