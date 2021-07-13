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

#include "backend.h"
#include <fcntl.h>

#ifndef HAVE__RB_FIBER_TRANSFER
static ID id_transfer;

VALUE
Event_Backend_fiber_transfer(VALUE fiber) {
	return rb_funcall(fiber, id_transfer, 0);
}

VALUE
Event_Backend_fiber_transfer_result(VALUE fiber, VALUE result) {
	return rb_funcall(fiber, id_transfer, 1, result);
}
#endif

#ifndef HAVE_RB_IO_DESCRIPTOR
static ID id_fileno;

int Event_Backend_io_descriptor(VALUE io) {
	return RB_NUM2INT(rb_funcall(io, id_fileno, 0));
}
#endif

#ifndef HAVE_RB_PROCESS_STATUS_WAIT
static ID id_wait;
static VALUE rb_Process_Status = Qnil;

VALUE Event_Backend_process_status_wait(rb_pid_t pid)
{
	return rb_funcall(rb_Process_Status, id_wait, 2, PIDT2NUM(pid), INT2NUM(WNOHANG));
}
#endif

int Event_Backend_nonblock_set(int file_descriptor)
{
	int flags = fcntl(file_descriptor, F_GETFL, 0);
	
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK);
	}
	
	return flags;
}

void Event_Backend_nonblock_restore(int file_descriptor, int flags)
{
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags & ~flags);
	}
}

void Init_Event_Backend(VALUE Event_Backend) {
#ifndef HAVE_RB_IO_DESCRIPTOR
	id_fileno = rb_intern("fileno");
#endif
	
#ifndef HAVE__RB_FIBER_TRANSFER
	id_transfer = rb_intern("transfer");
#endif
	
#ifndef HAVE_RB_PROCESS_STATUS_WAIT
	id_wait = rb_intern("wait");
	rb_Process_Status = rb_const_get_at(rb_mProcess, rb_intern("Status"));
#endif
}

struct wait_and_transfer_arguments {
	struct Event_Backend *backend;
	struct Event_Backend_Queue *waiting;
};

static void queue_pop(struct Event_Backend *backend, struct Event_Backend_Queue *waiting) {
	if (waiting->behind) {
		waiting->behind->infront = waiting->infront;
	} else {
		backend->waiting = waiting->infront;
	}
	
	if (waiting->infront) {
		waiting->infront->behind = waiting->behind;
	} else {
		backend->ready = waiting->behind;
	}
}

static void queue_push(struct Event_Backend *backend, struct Event_Backend_Queue *waiting) {
	if (backend->waiting) {
		backend->waiting->behind = waiting;
		waiting->infront = backend->waiting;
	} else {
		backend->ready = waiting;
	}
	
	backend->waiting = waiting;
}

static VALUE wait_and_transfer(VALUE fiber) {
	return Event_Backend_fiber_transfer(fiber);
}

static VALUE wait_and_transfer_ensure(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	queue_pop(arguments->backend, arguments->waiting);
	
	return Qnil;
}

void Event_Backend_wait_and_transfer(struct Event_Backend *backend, VALUE fiber)
{
	struct Event_Backend_Queue waiting = {
		.behind = NULL,
		.infront = NULL,
		.fiber = rb_fiber_current()
	};
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.backend = backend,
		.waiting = &waiting,
	};
	
	rb_ensure(wait_and_transfer, fiber, wait_and_transfer_ensure, (VALUE)&arguments);
}

void Event_Backend_ready_pop(struct Event_Backend *backend)
{
	// Get the current tail and head of the queue:
	struct Event_Backend_Queue *waiting = backend->waiting;
	
	// Process from head to tail in order:
	// During this, more items may be appended to tail.
	while (backend->ready) {
		struct Event_Backend_Queue *ready = backend->ready;
		
		Event_Backend_fiber_transfer(ready->fiber);
		
		if (ready == waiting) break;
	}
}

void Event_Backend_elapsed_time(struct timespec* start, struct timespec* stop, struct timespec *duration)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		duration->tv_sec = stop->tv_sec - start->tv_sec - 1;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
	} else {
		duration->tv_sec = stop->tv_sec - start->tv_sec;
		duration->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
}

void Event_Backend_current_time(struct timespec *time) {
	clock_gettime(CLOCK_MONOTONIC, time);
}
