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

#include "selector.h"
#include <fcntl.h>

static const int DEBUG = 0;

static ID id_transfer, id_alive_p;

VALUE IO_Event_Selector_fiber_transfer(VALUE fiber, int argc, VALUE *argv) {
	// TODO Consider introducing something like `rb_fiber_scheduler_transfer(...)`.
#ifdef HAVE__RB_FIBER_TRANSFER
	if (RTEST(rb_obj_is_fiber(fiber))) {
		if (RTEST(rb_fiber_alive_p(fiber))) {
			return rb_fiber_transfer(fiber, argc, argv);
		}
		
		return Qnil;
	}
#endif
	if (RTEST(rb_funcall(fiber, id_alive_p, 0))) {
		return rb_funcallv(fiber, id_transfer, argc, argv);
	}
	
	return Qnil;
}

#ifndef HAVE__RB_FIBER_RAISE
static ID id_raise;

VALUE IO_Event_Selector_fiber_raise(VALUE fiber, int argc, VALUE *argv) {
	return rb_funcallv(fiber, id_raise, argc, argv);
}
#endif

#ifndef HAVE_RB_FIBER_CURRENT
static ID id_current;

static VALUE rb_fiber_current() {
	return rb_funcall(rb_cFiber, id_current, 0);
}
#endif


#ifndef HAVE_RB_IO_DESCRIPTOR
static ID id_fileno;

int IO_Event_Selector_io_descriptor(VALUE io) {
	return RB_NUM2INT(rb_funcall(io, id_fileno, 0));
}
#endif

#ifndef HAVE_RB_PROCESS_STATUS_WAIT
static ID id_wait;
static VALUE rb_Process_Status = Qnil;

VALUE IO_Event_Selector_process_status_wait(rb_pid_t pid, int flags)
{
	return rb_funcall(rb_Process_Status, id_wait, 2, PIDT2NUM(pid), INT2NUM(flags | WNOHANG));
}
#endif

int IO_Event_Selector_nonblock_set(int file_descriptor)
{
#ifdef _WIN32
	u_long nonblock = 1;
	ioctlsocket(file_descriptor, FIONBIO, &nonblock);
	// Windows does not provide any way to know this, so we always restore it back to unset:
	return 0;
#else
	// Get the current mode:
	int flags = fcntl(file_descriptor, F_GETFL, 0);
	
	// Set the non-blocking flag if it isn't already:
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK);
	}
	
	return flags;
#endif
}

void IO_Event_Selector_nonblock_restore(int file_descriptor, int flags)
{
#ifdef _WIN32
	// Yolo...
	u_long nonblock = flags;
	ioctlsocket(file_descriptor, FIONBIO, &nonblock);
#else
	// The flags didn't have O_NONBLOCK set, so it would have been set, so we need to restore it:
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags);
	}
#endif
}

struct IO_Event_Selector_nonblock_arguments {
	int file_descriptor;
	int flags;
};

static VALUE IO_Event_Selector_nonblock_ensure(VALUE _arguments) {
	struct IO_Event_Selector_nonblock_arguments *arguments = (struct IO_Event_Selector_nonblock_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->file_descriptor, arguments->flags);
	
	return Qnil;
}

static VALUE IO_Event_Selector_nonblock(VALUE class, VALUE io)
{
	struct IO_Event_Selector_nonblock_arguments arguments = {
		.file_descriptor = IO_Event_Selector_io_descriptor(io),
		.flags = IO_Event_Selector_nonblock_set(arguments.file_descriptor)
	};
	
	return rb_ensure(rb_yield, io, IO_Event_Selector_nonblock_ensure, (VALUE)&arguments);
}

void Init_IO_Event_Selector(VALUE IO_Event_Selector) {
	id_transfer = rb_intern("transfer");
	id_alive_p = rb_intern("alive?");
	
#ifndef HAVE__RB_FIBER_RAISE
	id_raise = rb_intern("raise");
#endif
	
#ifndef HAVE_RB_FIBER_CURRENT
	id_current = rb_intern("current");
#endif
	
#ifndef HAVE_RB_IO_DESCRIPTOR
	id_fileno = rb_intern("fileno");
#endif
	
#ifndef HAVE_RB_PROCESS_STATUS_WAIT
	id_wait = rb_intern("wait");
	rb_Process_Status = rb_const_get_at(rb_mProcess, rb_intern("Status"));
	rb_gc_register_mark_object(rb_Process_Status);
#endif

	rb_define_singleton_method(IO_Event_Selector, "nonblock", IO_Event_Selector_nonblock, 1);
}

struct wait_and_transfer_arguments {
	int argc;
	VALUE *argv;
	
	struct IO_Event_Selector *backend;
	struct IO_Event_Selector_Queue *waiting;
};

static void queue_pop(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *waiting) {
	if (waiting->head) {
		waiting->head->tail = waiting->tail;
	} else {
		backend->waiting = waiting->tail;
	}
	
	if (waiting->tail) {
		waiting->tail->head = waiting->head;
	} else {
		backend->ready = waiting->head;
	}
}

static void queue_push(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *waiting) {
	if (backend->waiting) {
		backend->waiting->head = waiting;
		waiting->tail = backend->waiting;
	} else {
		backend->ready = waiting;
	}
	
	backend->waiting = waiting;
}

static VALUE wait_and_transfer(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return IO_Event_Selector_fiber_transfer(fiber, argc, argv);
}

static VALUE wait_and_transfer_ensure(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	queue_pop(arguments->backend, arguments->waiting);
	
	return Qnil;
}

VALUE IO_Event_Selector_resume(struct IO_Event_Selector *backend, int argc, VALUE *argv)
{
	rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);
	
	struct IO_Event_Selector_Queue waiting = {
		.head = NULL,
		.tail = NULL,
		.flags = IO_EVENT_SELECTOR_QUEUE_FIBER,
		.fiber = rb_fiber_current()
	};
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_transfer, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

static VALUE wait_and_raise(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return IO_Event_Selector_fiber_raise(fiber, argc, argv);
}

VALUE IO_Event_Selector_raise(struct IO_Event_Selector *backend, int argc, VALUE *argv)
{
	rb_check_arity(argc, 2, UNLIMITED_ARGUMENTS);
	
	struct IO_Event_Selector_Queue waiting = {
		.head = NULL,
		.tail = NULL,
		.flags = IO_EVENT_SELECTOR_QUEUE_FIBER,
		.fiber = rb_fiber_current()
	};
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_raise, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

void IO_Event_Selector_queue_push(struct IO_Event_Selector *backend, VALUE fiber)
{
	struct IO_Event_Selector_Queue *waiting = malloc(sizeof(struct IO_Event_Selector_Queue));
	
	waiting->head = NULL;
	waiting->tail = NULL;
	waiting->flags = IO_EVENT_SELECTOR_QUEUE_INTERNAL;
	waiting->fiber = fiber;
	
	queue_push(backend, waiting);
}

static inline
void IO_Event_Selector_queue_pop(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *ready)
{
	if (DEBUG) fprintf(stderr, "IO_Event_Selector_queue_pop -> %p\n", (void*)ready->fiber);
	if (ready->flags & IO_EVENT_SELECTOR_QUEUE_FIBER) {
		IO_Event_Selector_fiber_transfer(ready->fiber, 0, NULL);
	} else {
		VALUE fiber = ready->fiber;
		queue_pop(backend, ready);
		free(ready);
		
		if (RTEST(rb_funcall(fiber, id_alive_p, 0))) {
			rb_funcall(fiber, id_transfer, 0);
		}
	}
}

int IO_Event_Selector_queue_flush(struct IO_Event_Selector *backend)
{
	int count = 0;
	
	// Get the current tail and head of the queue:
	struct IO_Event_Selector_Queue *waiting = backend->waiting;
	if (DEBUG) fprintf(stderr, "IO_Event_Selector_queue_flush waiting = %p\n", waiting);
	
	// Process from head to tail in order:
	// During this, more items may be appended to tail.
	while (backend->ready) {
		if (DEBUG) fprintf(stderr, "backend->ready = %p\n", backend->ready);
		struct IO_Event_Selector_Queue *ready = backend->ready;
		
		count += 1;
		IO_Event_Selector_queue_pop(backend, ready);
		
		if (ready == waiting) break;
	}
	
	return count;
}

void IO_Event_Selector_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		duration->tv_sec = stop->tv_sec - start->tv_sec - 1;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		duration->tv_sec = stop->tv_sec - start->tv_sec;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
}

void IO_Event_Selector_current_time(struct timespec *time) {
	clock_gettime(CLOCK_MONOTONIC, time);
}
