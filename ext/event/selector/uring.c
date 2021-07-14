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

#include "uring.h"
#include "selector.h"

#include <liburing.h>
#include <poll.h>
#include <time.h>

#include "pidfd.c"

static const int DEBUG = 0;

static VALUE Event_Selector_URing = Qnil;

enum {URING_ENTRIES = 64};

struct Event_Selector_URing {
	struct Event_Selector backend;
	struct io_uring ring;
	size_t pending;
};

void Event_Selector_URing_Type_mark(void *_data)
{
	struct Event_Selector_URing *data = _data;
	Event_Selector_mark(&data->backend);
}

static
void close_internal(struct Event_Selector_URing *data) {
	if (data->ring.ring_fd >= 0) {
		io_uring_queue_exit(&data->ring);
		data->ring.ring_fd = -1;
	}
}

void Event_Selector_URing_Type_free(void *_data)
{
	struct Event_Selector_URing *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t Event_Selector_URing_Type_size(const void *data)
{
	return sizeof(struct Event_Selector_URing);
}

static const rb_data_type_t Event_Selector_URing_Type = {
	.wrap_struct_name = "Event::Backend::URing",
	.function = {
		.dmark = Event_Selector_URing_Type_mark,
		.dfree = Event_Selector_URing_Type_free,
		.dsize = Event_Selector_URing_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Selector_URing_allocate(VALUE self) {
	struct Event_Selector_URing *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	Event_Selector_initialize(&data->backend, Qnil);
	data->ring.ring_fd = -1;
	
	data->pending = 0;

	return instance;
}

VALUE Event_Selector_URing_initialize(VALUE self, VALUE loop) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	Event_Selector_initialize(&data->backend, loop);
	int result = io_uring_queue_init(URING_ENTRIES, &data->ring, 0);
	
	if (result < 0) {
		rb_syserr_fail(-result, "io_uring_queue_init");
	}
	
	rb_update_max_fd(data->ring.ring_fd);
	
	return self;
}

VALUE Event_Selector_URing_close(VALUE self) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE Event_Selector_URing_transfer(int argc, VALUE *argv, VALUE self)
{
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	Event_Selector_wait_and_transfer(&data->backend, argc, argv);
	
	return Qnil;
}

VALUE Event_Selector_URing_yield(VALUE self)
{
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	Event_Selector_yield(&data->backend);
	
	return Qnil;
}

VALUE Event_Selector_URing_push(VALUE self, VALUE fiber)
{
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	Event_Selector_queue_push(&data->backend, fiber);
	
	return Qnil;
}

VALUE Event_Selector_URing_raise(int argc, VALUE *argv, VALUE self)
{
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	return Event_Selector_wait_and_raise(&data->backend, argc, argv);
}

VALUE Event_Selector_URing_ready_p(VALUE self) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

static
int io_uring_submit_flush(struct Event_Selector_URing *data) {
	if (data->pending) {
		if (DEBUG) fprintf(stderr, "io_uring_submit_flush(pending=%ld)\n", data->pending);
		
		// Try to submit:
		int result = io_uring_submit(&data->ring);

		if (result >= 0) {
			// If it was submitted, reset pending count:
			data->pending = 0;
		} else if (result != -EBUSY && result != -EAGAIN) {
			rb_syserr_fail(-result, "io_uring_submit_flush");
		}

		return result;
	}
	
	return 0;
}

static
int io_uring_submit_now(struct Event_Selector_URing *data) {
	while (true) {
		int result = io_uring_submit(&data->ring);
		
		if (result >= 0) {
			data->pending = 0;
			return result;
		}

		if (result == -EBUSY || result == -EAGAIN) {
			Event_Selector_yield(&data->backend);
		} else {
			rb_syserr_fail(-result, "io_uring_submit_now");
		}
	}
}

static
void io_uring_submit_pending(struct Event_Selector_URing *data) {
	data->pending += 1;
}

struct io_uring_sqe * io_get_sqe(struct Event_Selector_URing *data) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	while (sqe == NULL) {
		// The submit queue is full, we need to drain it:	
		io_uring_submit_now(data);

		sqe = io_uring_get_sqe(&data->ring);
	}
	
	return sqe;
}

struct process_wait_arguments {
	struct Event_Selector_URing *data;
	pid_t pid;
	int flags;
	int descriptor;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	close(arguments->descriptor);
	
	return Qnil;
}

VALUE Event_Selector_URing_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = NUM2INT(flags),
	};
	
	process_wait_arguments.descriptor = pidfd_open(process_wait_arguments.pid, 0);
	rb_update_max_fd(process_wait_arguments.descriptor);
	
	struct io_uring_sqe *sqe = io_get_sqe(data);

	if (DEBUG) fprintf(stderr, "Event_Selector_URing_process_wait:io_uring_prep_poll_add(%p)\n", (void*)fiber);
	io_uring_prep_poll_add(sqe, process_wait_arguments.descriptor, POLLIN|POLLHUP|POLLERR);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_pending(data);

	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

static inline
short poll_flags_from_events(int events) {
	short flags = 0;
	
	if (events & EVENT_READABLE) flags |= POLLIN;
	if (events & EVENT_PRIORITY) flags |= POLLPRI;
	if (events & EVENT_WRITABLE) flags |= POLLOUT;
	
	flags |= POLLERR;
	flags |= POLLHUP;
	
	return flags;
}

static inline
int events_from_poll_flags(short flags) {
	int events = 0;
	
	if (flags & POLLIN) events |= EVENT_READABLE;
	if (flags & POLLPRI) events |= EVENT_PRIORITY;
	if (flags & POLLOUT) events |= EVENT_WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct Event_Selector_URing *data;
	VALUE fiber;
	short flags;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Selector_URing *data = arguments->data;
	
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	if (DEBUG) fprintf(stderr, "io_wait_rescue:io_uring_prep_poll_remove(%p)\n", (void*)arguments->fiber);
	
	io_uring_prep_poll_remove(sqe, (void*)arguments->fiber);
	io_uring_submit_now(data);

	rb_exc_raise(exception);
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Selector_URing *data = arguments->data;

	VALUE result = Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
	if (DEBUG) fprintf(stderr, "io_wait:Event_Selector_fiber_transfer -> %d\n", RB_NUM2INT(result));

	// We explicitly filter the resulting events based on the requested events.
	// In some cases, poll will report events we didn't ask for.
	short flags = arguments->flags & NUM2INT(result);
	
	return INT2NUM(events_from_poll_flags(flags));
};

VALUE Event_Selector_URing_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	short flags = poll_flags_from_events(NUM2INT(events));
	
	if (DEBUG) fprintf(stderr, "Event_Selector_URing_io_wait:io_uring_prep_poll_add(descriptor=%d, flags=%d, fiber=%p)\n", descriptor, flags, (void*)fiber);
	
	io_uring_prep_poll_add(sqe, descriptor, flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	
	// If we are going to wait, we assume that we are waiting for a while:
	io_uring_submit_pending(data);
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.fiber = fiber,
		.flags = flags
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

static int io_read(struct Event_Selector_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	struct io_uring_sqe *sqe = io_get_sqe(data);

	if (DEBUG) fprintf(stderr, "io_read:io_uring_prep_read(fiber=%p)\n", (void*)fiber);

	io_uring_prep_read(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_now(data);
	
	VALUE result = Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
	if (DEBUG) fprintf(stderr, "io_read:Event_Selector_fiber_transfer -> %d\n", RB_NUM2INT(result));

	return RB_NUM2INT(result);
}

VALUE Event_Selector_URing_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	
	void *base;
	size_t size;
	rb_io_buffer_get_mutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	while (length > 0) {
		size_t maximum_size = size - offset;
		int result = io_read(data, fiber, descriptor, (char*)base+offset, maximum_size);
		
		if (result == 0) {
			break;
		} else if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			Event_Selector_URing_io_wait(self, fiber, io, RB_INT2NUM(EVENT_READABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

static
int io_write(struct Event_Selector_URing *data, VALUE fiber, int descriptor, char *buffer, size_t length) {
	struct io_uring_sqe *sqe = io_get_sqe(data);
	
	if (DEBUG) fprintf(stderr, "io_write:io_uring_prep_write(fiber=%p)\n", (void*)fiber);

	io_uring_prep_write(sqe, descriptor, buffer, length, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit_pending(data);
	
	int result = RB_NUM2INT(Event_Selector_fiber_transfer(data->backend.loop, 0, NULL));
	if (DEBUG) fprintf(stderr, "io_write:Event_Selector_fiber_transfer -> %d\n", result);

	return result;
}

VALUE Event_Selector_URing_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	
	const void *base;
	size_t size;
	rb_io_buffer_get_immutable(buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = NUM2SIZET(_length);
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (length > 0) {
		int result = io_write(data, fiber, descriptor, (char*)base+offset, length);
		
		if (result >= 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (-result == EAGAIN || -result == EWOULDBLOCK) {
			Event_Selector_URing_io_wait(self, fiber, io, RB_INT2NUM(EVENT_WRITABLE));
		} else {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	return SIZET2NUM(offset);
}

#endif

static const int ASYNC_CLOSE = 1;

VALUE Event_Selector_URing_io_close(VALUE self, VALUE io) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);

	if (ASYNC_CLOSE) {
		struct io_uring_sqe *sqe = io_get_sqe(data);
		
		io_uring_prep_close(sqe, descriptor);
		io_uring_sqe_set_data(sqe, NULL);
		io_uring_submit_now(data);
	} else {
		close(descriptor);
	}

	// We don't wait for the result of close since it has no use in pratice:
	return Qtrue;
}

static
struct __kernel_timespec * make_timeout(VALUE duration, struct __kernel_timespec *storage) {
	if (duration == Qnil) {
		return NULL;
	}
	
	if (FIXNUM_P(duration)) {
		storage->tv_sec = NUM2TIMET(duration);
		storage->tv_nsec = 0;
		
		return storage;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		time_t seconds = value;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

static
int timeout_nonblocking(struct __kernel_timespec *timespec) {
	return timespec && timespec->tv_sec == 0 && timespec->tv_nsec == 0;
}

struct select_arguments {
	struct Event_Selector_URing *data;
	
	int result;
	
	struct __kernel_timespec storage;
	struct __kernel_timespec *timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;

	io_uring_submit_flush(arguments->data);

	struct io_uring_cqe *cqe = NULL;
	arguments->result = io_uring_wait_cqe_timeout(&arguments->data->ring, &cqe, arguments->timeout);
	
	return NULL;
}

static
int select_internal_without_gvl(struct select_arguments *arguments) {
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	
	if (arguments->result == -ETIME) {
		arguments->result = 0;
	} else if (arguments->result < 0) {
		rb_syserr_fail(-arguments->result, "select_internal_without_gvl:io_uring_wait_cqes");
	} else {
		// At least 1 event is waiting:
		arguments->result = 1;
	}
	
	return arguments->result;
}

static inline
unsigned select_process_completions(struct io_uring *ring) {
	unsigned completed = 0;
	unsigned head;
	struct io_uring_cqe *cqe;
	
	io_uring_for_each_cqe(ring, head, cqe) {
		++completed;
		
		// If the operation was cancelled, or the operation has no user data (fiber):
		if (cqe->res == -ECANCELED || cqe->user_data == 0 || cqe->user_data == LIBURING_UDATA_TIMEOUT) {
			io_uring_cq_advance(ring, 1);
			continue;
		}
		
		VALUE fiber = (VALUE)cqe->user_data;
		VALUE result = RB_INT2NUM(cqe->res);
		
		if (DEBUG) fprintf(stderr, "cqe res=%d user_data=%p\n", cqe->res, (void*)cqe->user_data);
		
		io_uring_cq_advance(ring, 1);
		
		Event_Selector_fiber_transfer(fiber, 1, &result);
	}
	
	// io_uring_cq_advance(ring, completed);
	
	if (DEBUG) fprintf(stderr, "select_process_completions(completed=%d)\n", completed);
	
	return completed;
}

VALUE Event_Selector_URing_select(VALUE self, VALUE duration) {
	struct Event_Selector_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_URing, &Event_Selector_URing_Type, data);
	
	int ready = Event_Selector_queue_flush(&data->backend);
	
	int result = select_process_completions(&data->ring);

	// If the ready list was empty and we didn't process any completions:
	if (!ready && result == 0) {
		// We might need to wait for events:
		struct select_arguments arguments = {
			.data = data,
			.timeout = NULL,
		};
		
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!data->backend.ready && !timeout_nonblocking(arguments.timeout)) {
			// This is a blocking operation, we wait for events:
			result = select_internal_without_gvl(&arguments);
		} else {
			// The timeout specified required "nonblocking" behaviour so we just flush the SQ if required:
			io_uring_submit_flush(data);
		}

		// After waiting/flushing the SQ, check if there are any completions:
		result = select_process_completions(&data->ring);
	}
	
	return RB_INT2NUM(result);
}

void Init_Event_Selector_URing(VALUE Event_Selector) {
	Event_Selector_URing = rb_define_class_under(Event_Selector, "URing", rb_cObject);
	
	rb_define_alloc_func(Event_Selector_URing, Event_Selector_URing_allocate);
	rb_define_method(Event_Selector_URing, "initialize", Event_Selector_URing_initialize, 1);
	
	rb_define_method(Event_Selector_URing, "transfer", Event_Selector_URing_transfer, -1);
	rb_define_method(Event_Selector_URing, "yield", Event_Selector_URing_yield, 0);
	rb_define_method(Event_Selector_URing, "push", Event_Selector_URing_push, 1);
	rb_define_method(Event_Selector_URing, "raise", Event_Selector_URing_raise, -1);
	
	rb_define_method(Event_Selector_URing, "ready?", Event_Selector_URing_ready_p, 0);
	
	rb_define_method(Event_Selector_URing, "select", Event_Selector_URing_select, 1);
	rb_define_method(Event_Selector_URing, "close", Event_Selector_URing_close, 0);
	
	rb_define_method(Event_Selector_URing, "io_wait", Event_Selector_URing_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(Event_Selector_URing, "io_read", Event_Selector_URing_io_read, 4);
	rb_define_method(Event_Selector_URing, "io_write", Event_Selector_URing_io_write, 4);
#endif
	
	rb_define_method(Event_Selector_URing, "io_close", Event_Selector_URing_io_close, 1);
	
	rb_define_method(Event_Selector_URing, "process_wait", Event_Selector_URing_process_wait, 3);
}
