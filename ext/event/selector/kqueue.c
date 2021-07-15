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

#include "kqueue.h"
#include "selector.h"

#include <sys/event.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

static VALUE Event_Selector_KQueue = Qnil;

enum {KQUEUE_MAX_EVENTS = 64};

struct Event_Selector_KQueue {
	struct Event_Selector backend;
	int descriptor;
};

void Event_Selector_KQueue_Type_mark(void *_data)
{
	struct Event_Selector_KQueue *data = _data;
	Event_Selector_mark(&data->backend);
}

static
void close_internal(struct Event_Selector_KQueue *data) {
	if (data->descriptor >= 0) {
		close(data->descriptor);
		data->descriptor = -1;
	}
}

void Event_Selector_KQueue_Type_free(void *_data)
{
	struct Event_Selector_KQueue *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t Event_Selector_KQueue_Type_size(const void *data)
{
	return sizeof(struct Event_Selector_KQueue);
}

static const rb_data_type_t Event_Selector_KQueue_Type = {
	.wrap_struct_name = "Event::Backend::KQueue",
	.function = {
		.dmark = Event_Selector_KQueue_Type_mark,
		.dfree = Event_Selector_KQueue_Type_free,
		.dsize = Event_Selector_KQueue_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Selector_KQueue_allocate(VALUE self) {
	struct Event_Selector_KQueue *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	Event_Selector_initialize(&data->backend, Qnil);
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Selector_KQueue_initialize(VALUE self, VALUE loop) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	Event_Selector_initialize(&data->backend, loop);
	int result = kqueue();
	
	if (result == -1) {
		rb_sys_fail("kqueue");
	} else {
		ioctl(result, FIOCLEX);
		data->descriptor = result;
		
		rb_update_max_fd(data->descriptor);
	}
	
	return self;
}

VALUE Event_Selector_KQueue_close(VALUE self) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE Event_Selector_KQueue_resume(int argc, VALUE *argv, VALUE self)
{
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	return Event_Selector_resume(&data->backend, argc, argv);
}

VALUE Event_Selector_KQueue_yield(VALUE self)
{
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	Event_Selector_yield(&data->backend);
	
	return Qnil;
}

VALUE Event_Selector_KQueue_push(VALUE self, VALUE fiber)
{
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	Event_Selector_queue_push(&data->backend, fiber);
	
	return Qnil;
}

VALUE Event_Selector_KQueue_raise(int argc, VALUE *argv, VALUE self)
{
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	return Event_Selector_wait_and_raise(&data->backend, argc, argv);
}

VALUE Event_Selector_KQueue_ready_p(VALUE self) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct Event_Selector_KQueue *data;
	pid_t pid;
	int flags;
};

static
int process_add_filters(int descriptor, int ident, VALUE fiber) {
	struct kevent event = {0};
	
	event.ident = ident;
	event.filter = EVFILT_PROC;
	event.flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
	event.fflags = NOTE_EXIT;
	event.udata = (void*)fiber;
	
	int result = kevent(descriptor, &event, 1, NULL, 0, NULL);
	
	if (result == -1) {
		// No such process - the process has probably already terminated:
		if (errno == ESRCH) {
			return 0;
		}
		
		rb_sys_fail("kevent(process_add_filters)");
	}
	
	return 1;
}

static
void process_remove_filters(int descriptor, int ident) {
	struct kevent event = {0};
	
	event.ident = ident;
	event.filter = EVFILT_PROC;
	event.flags = EV_DELETE;
	event.fflags = NOTE_EXIT;
	
	// Ignore the result.
	kevent(descriptor, &event, 1, NULL, 0, NULL);
}

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_rescue(VALUE _arguments, VALUE exception) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	process_remove_filters(arguments->data->descriptor, arguments->pid);
	
	rb_exc_raise(exception);
}

VALUE Event_Selector_KQueue_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = RB_NUM2INT(flags),
	};
	
	int waiting = process_add_filters(data->descriptor, process_wait_arguments.pid, fiber);
	
	if (waiting) {
		return rb_rescue(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_rescue, (VALUE)&process_wait_arguments);
	} else {
		return Event_Selector_process_status_wait(process_wait_arguments.pid);
	}
}

static
int io_add_filters(int descriptor, int ident, int events, VALUE fiber) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & EVENT_READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].udata = (void*)fiber;
		
// #ifdef EV_OOBAND
// 		if (events & PRIORITY) {
// 			kevents[count].flags |= EV_OOBAND;
// 		}
// #endif
		
		count++;
	}
	
	if (events & EVENT_WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].udata = (void*)fiber;
		count++;
	}
	
	int result = kevent(descriptor, kevents, count, NULL, 0, NULL);
	
	if (result == -1) {
		rb_sys_fail("kevent(io_add_filters)");
	}
	
	return events;
}

static
void io_remove_filters(int descriptor, int ident, int events) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & EVENT_READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_DELETE;
		
		count++;
	}
	
	if (events & EVENT_WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_DELETE;
		count++;
	}
	
	// Ignore the result.
	kevent(descriptor, kevents, count, NULL, 0, NULL);
}

struct io_wait_arguments {
	struct Event_Selector_KQueue *data;
	int events;
	int descriptor;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	io_remove_filters(arguments->data->descriptor, arguments->descriptor, arguments->events);
	
	rb_exc_raise(exception);
}

static inline
int events_from_kqueue_filter(int filter) {
	if (filter == EVFILT_READ) return EVENT_READABLE;
	if (filter == EVFILT_WRITE) return EVENT_WRITABLE;
	
	return 0;
}

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return INT2NUM(events_from_kqueue_filter(RB_NUM2INT(result)));
}

VALUE Event_Selector_KQueue_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	
	struct io_wait_arguments io_wait_arguments = {
		.events = io_add_filters(data->descriptor, descriptor, RB_NUM2INT(events), fiber),
		.data = data,
		.descriptor = descriptor,
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

struct io_read_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int descriptor;
	
	VALUE buffer;
	size_t length;
};

static
VALUE io_read_loop(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	void *base;
	size_t size;
	rb_io_buffer_get_mutable(arguments->buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = arguments->length;
	
	while (length > 0) {
		size_t maximum_size = size - offset;
		ssize_t result = read(arguments->descriptor, (char*)base+offset, maximum_size);
		
		if (result == 0) {
			break;
		} else if (result > 0) {
			offset += result;
			length -= result;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(EVENT_READABLE));
		} else {
			rb_sys_fail("Event_Selector_KQueue_io_read");
		}
	}
	
	return SIZET2NUM(offset);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE Event_Selector_KQueue_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	
	struct io_read_arguments io_read_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
	};
	
	return rb_ensure(io_read_loop, (VALUE)&io_read_arguments, io_read_ensure, (VALUE)&io_read_arguments);
}

struct io_write_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int descriptor;
	
	VALUE buffer;
	size_t length;
};

static
VALUE io_write_loop(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	const void *base;
	size_t size;
	rb_io_buffer_get_immutable(arguments->buffer, &base, &size);
	
	size_t offset = 0;
	size_t length = arguments->length;
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (length > 0) {
		ssize_t result = write(arguments->descriptor, (char*)base+offset, length);
		
		if (result >= 0) {
			offset += result;
			length -= result;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(EVENT_WRITABLE));
		} else {
			rb_sys_fail("Event_Selector_KQueue_io_write");
		}
	}
	
	return SIZET2NUM(offset);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE Event_Selector_KQueue_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	int descriptor = Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	
	struct io_write_arguments io_write_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
	};
	
	return rb_ensure(io_write_loop, (VALUE)&io_write_arguments, io_write_ensure, (VALUE)&io_write_arguments);
}

#endif

static
struct timespec * make_timeout(VALUE duration, struct timespec * storage) {
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
int timeout_nonblocking(struct timespec * timespec) {
	return timespec && timespec->tv_sec == 0 && timespec->tv_nsec == 0;
}

struct select_arguments {
	struct Event_Selector_KQueue *data;
	
	int count;
	struct kevent events[KQUEUE_MAX_EVENTS];
	
	struct timespec storage;
	struct timespec *timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
	arguments->count = kevent(arguments->data->descriptor, NULL, 0, arguments->events, arguments->count, arguments->timeout);
	
	return NULL;
}

static
void select_internal_without_gvl(struct select_arguments *arguments) {
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	
	if (arguments->count == -1) {
		rb_sys_fail("select_internal_without_gvl:kevent");
	}
}

static
void select_internal_with_gvl(struct select_arguments *arguments) {
	select_internal((void *)arguments);
	
	if (arguments->count == -1) {
		rb_sys_fail("select_internal_with_gvl:kevent");
	}
}

VALUE Event_Selector_KQueue_select(VALUE self, VALUE duration) {
	struct Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Selector_KQueue, &Event_Selector_KQueue_Type, data);
	
	int ready = Event_Selector_queue_flush(&data->backend);
	
	struct select_arguments arguments = {
		.data = data,
		.count = KQUEUE_MAX_EVENTS,
		.storage = {
			.tv_sec = 0,
			.tv_nsec = 0
		}
	};
	
	// We break this implementation into two parts.
	// (1) count = kevent(..., timeout = 0)
	// (2) without gvl: kevent(..., timeout = 0) if count == 0 and timeout != 0
	// This allows us to avoid releasing and reacquiring the GVL.
	// Non-comprehensive testing shows this gives a 1.5x speedup.
	arguments.timeout = &arguments.storage;
	
	// First do the syscall with no timeout to get any immediately available events:
	select_internal_with_gvl(&arguments);
	
	// If there were no pending events, if we have a timeout, wait for more events:
	if (!ready && arguments.count == 0) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			arguments.count = KQUEUE_MAX_EVENTS;
			
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		VALUE fiber = (VALUE)arguments.events[i].udata;
		VALUE result = INT2NUM(arguments.events[i].filter);
		
		Event_Selector_fiber_transfer(fiber, 1, &result);
	}
	
	return INT2NUM(arguments.count);
}

void Init_Event_Selector_KQueue(VALUE Event_Selector) {
	Event_Selector_KQueue = rb_define_class_under(Event_Selector, "KQueue", rb_cObject);
	
	rb_define_alloc_func(Event_Selector_KQueue, Event_Selector_KQueue_allocate);
	rb_define_method(Event_Selector_KQueue, "initialize", Event_Selector_KQueue_initialize, 1);
	
	rb_define_method(Event_Selector_KQueue, "resume", Event_Selector_KQueue_resume, -1);
	rb_define_method(Event_Selector_KQueue, "yield", Event_Selector_KQueue_yield, 0);
	rb_define_method(Event_Selector_KQueue, "push", Event_Selector_KQueue_push, 1);
	rb_define_method(Event_Selector_KQueue, "raise", Event_Selector_KQueue_raise, -1);
	
	rb_define_method(Event_Selector_KQueue, "ready?", Event_Selector_KQueue_ready_p, 0);
	
	rb_define_method(Event_Selector_KQueue, "select", Event_Selector_KQueue_select, 1);
	rb_define_method(Event_Selector_KQueue, "close", Event_Selector_KQueue_close, 0);
	
	rb_define_method(Event_Selector_KQueue, "io_wait", Event_Selector_KQueue_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(Event_Selector_KQueue, "io_read", Event_Selector_KQueue_io_read, 4);
	rb_define_method(Event_Selector_KQueue, "io_write", Event_Selector_KQueue_io_write, 4);
#endif
	
	rb_define_method(Event_Selector_KQueue, "process_wait", Event_Selector_KQueue_process_wait, 3);
}
