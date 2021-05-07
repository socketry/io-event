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
#include "backend.h"

#include <sys/event.h>
#include <sys/ioctl.h>
#include <time.h>

static VALUE Event_Backend_KQueue = Qnil;
static ID id_fileno, id_transfer;

enum {KQUEUE_MAX_EVENTS = 64};

struct Event_Backend_KQueue {
	VALUE loop;
	int descriptor;
};

void Event_Backend_KQueue_Type_mark(void *_data)
{
	struct Event_Backend_KQueue *data = _data;
	rb_gc_mark(data->loop);
}

static
void close_internal(struct Event_Backend_KQueue *data) {
	if (data->descriptor >= 0) {
		close(data->descriptor);
		data->descriptor = -1;
	}
}

void Event_Backend_KQueue_Type_free(void *_data)
{
	struct Event_Backend_KQueue *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t Event_Backend_KQueue_Type_size(const void *data)
{
	return sizeof(struct Event_Backend_KQueue);
}

static const rb_data_type_t Event_Backend_KQueue_Type = {
	.wrap_struct_name = "Event::Backend::KQueue",
	.function = {
		.dmark = Event_Backend_KQueue_Type_mark,
		.dfree = Event_Backend_KQueue_Type_free,
		.dsize = Event_Backend_KQueue_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Backend_KQueue_allocate(VALUE self) {
	struct Event_Backend_KQueue *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	data->loop = Qnil;
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Backend_KQueue_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	data->loop = loop;
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

VALUE Event_Backend_KQueue_close(VALUE self) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

static
int io_add_filters(int descriptor, int ident, int events, VALUE fiber) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & READABLE) {
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
	
	if (events & WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		kevents[count].udata = (void*)fiber;
		count++;
	}
	
	int result = kevent(descriptor, kevents, count, NULL, 0, NULL);
	
	if (result == -1) {
		rb_sys_fail("kevent(register)");
	}
	
	return events;
}

static
void io_remove_filters(int descriptor, int ident, int events) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_DELETE;
		
		count++;
	}
	
	if (events & WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_DELETE;
		count++;
	}
	
	// Ignore the result.
	kevent(descriptor, kevents, count, NULL, 0, NULL);
}

struct io_wait_arguments {
	struct Event_Backend_KQueue *data;
	int events;
	int descriptor;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	io_remove_filters(arguments->data->descriptor, arguments->descriptor, arguments->events);
	
	rb_exc_raise(exception);
};

static inline
int events_from_kqueue_filter(int filter) {
	if (filter == EVFILT_READ) return READABLE;
	if (filter == EVFILT_WRITE) return WRITABLE;
	
	return 0;
}

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = rb_funcall(arguments->data->loop, id_transfer, 0);
	
	return INT2NUM(events_from_kqueue_filter(NUM2INT(result)));
};

VALUE Event_Backend_KQueue_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	
	struct io_wait_arguments io_wait_arguments = {
		.events = io_add_filters(data->descriptor, descriptor, NUM2INT(events), fiber),
		.data = data,
		.descriptor = descriptor,
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

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
	struct Event_Backend_KQueue *data;
	
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

VALUE Event_Backend_KQueue_select(VALUE self, VALUE duration) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
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
	if (arguments.count == 0) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			arguments.count = KQUEUE_MAX_EVENTS;
			
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		VALUE fiber = (VALUE)arguments.events[i].udata;
		VALUE result = INT2NUM(arguments.events[i].filter);
		rb_funcall(fiber, id_transfer, 1, result);
	}
	
	return INT2NUM(arguments.count);
}

void Init_Event_Backend_KQueue(VALUE Event_Backend) {
	id_fileno = rb_intern("fileno");
	id_transfer = rb_intern("transfer");
	
	Event_Backend_KQueue = rb_define_class_under(Event_Backend, "KQueue", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_KQueue, Event_Backend_KQueue_allocate);
	rb_define_method(Event_Backend_KQueue, "initialize", Event_Backend_KQueue_initialize, 1);
	rb_define_method(Event_Backend_KQueue, "close", Event_Backend_KQueue_close, 0);
	
	rb_define_method(Event_Backend_KQueue, "io_wait", Event_Backend_KQueue_io_wait, 3);
	rb_define_method(Event_Backend_KQueue, "select", Event_Backend_KQueue_select, 1);
}
