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

#include <sys/epoll.h>
#include <time.h>
#include <errno.h>

static VALUE Event_Backend_EPoll = Qnil;
static ID id_fileno, id_transfer;

static const unsigned EPOLL_MAX_EVENTS = 64;

struct Event_Backend_EPoll {
	VALUE loop;
	int descriptor;
};

void Event_Backend_EPoll_Type_mark(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	rb_gc_mark(data->loop);
}

void Event_Backend_EPoll_Type_free(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	
	if (data->descriptor >= 0) {
		close(data->descriptor);
	}
	
	free(data);
}

size_t Event_Backend_EPoll_Type_size(const void *data)
{
	return sizeof(struct Event_Backend_EPoll);
}

static const rb_data_type_t Event_Backend_EPoll_Type = {
	.wrap_struct_name = "Event::Backend::EPoll",
	.function = {
		.dmark = Event_Backend_EPoll_Type_mark,
		.dfree = Event_Backend_EPoll_Type_free,
		.dsize = Event_Backend_EPoll_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Backend_EPoll_allocate(VALUE self) {
	struct Event_Backend_EPoll *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	data->loop = Qnil;
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Backend_EPoll_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	data->loop = loop;
	int result = epoll_create1(EPOLL_CLOEXEC);
	
	if (result == -1) {
		rb_sys_fail("epoll_create");
	} else {
		data->descriptor = result;
		
		rb_update_max_fd(data->descriptor);
	}
	
	return self;
}

static inline
uint32_t epoll_flags_from_events(int events) {
	uint32_t flags = 0;
	
	if (events & READABLE) flags |= EPOLLIN;
	if (events & PRIORITY) flags |= EPOLLPRI;
	if (events & WRITABLE) flags |= EPOLLOUT;
	
	flags |= EPOLLRDHUP;
	flags |= EPOLLONESHOT;
	
	return flags;
}

static inline
int events_from_epoll_flags(uint32_t flags) {
	int events = 0;
	
	if (flags & EPOLLIN) events |= READABLE;
	if (flags & EPOLLPRI) events |= PRIORITY;
	if (flags & EPOLLOUT) events |= WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct Event_Backend_EPoll *data;
	int descriptor;
	int duplicate;
};

static
VALUE io_wait_ensure(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	if (arguments->duplicate >= 0) {
		epoll_ctl(arguments->data->descriptor, EPOLL_CTL_DEL, arguments->duplicate, NULL);
		
		close(arguments->duplicate);
	} else {
		epoll_ctl(arguments->data->descriptor, EPOLL_CTL_DEL, arguments->descriptor, NULL);
	}
	
	return Qnil;
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = rb_funcall(arguments->data->loop, id_transfer, 0);
	
	return INT2NUM(events_from_epoll_flags(NUM2INT(result)));
};

VALUE Event_Backend_EPoll_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event event = {0};
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	int duplicate = -1;
	
	event.events = epoll_flags_from_events(NUM2INT(events));
	event.data.ptr = (void*)fiber;
	
	// fprintf(stderr, "<- fiber=%p descriptor=%d\n", (void*)fiber, descriptor);
	
	// A better approach is to batch all changes:
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1 && errno == EEXIST) {
		// The file descriptor was already inserted into epoll.
		duplicate = descriptor = dup(descriptor);
		
		rb_update_max_fd(duplicate);
		
		if (descriptor == -1)
			rb_sys_fail("dup");
		
		result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	}
	
	if (result == -1) {
		rb_sys_fail("epoll_ctl");
	}
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.descriptor = descriptor,
		.duplicate = duplicate
	};
	
	return rb_ensure(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_ensure, (VALUE)&io_wait_arguments);
}

static
int make_timeout(VALUE duration) {
	if (duration == Qnil) {
		return -1;
	}
	
	if (FIXNUM_P(duration)) {
		return NUM2LONG(duration) * 1000L;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		
		return value * 1000;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

VALUE Event_Backend_EPoll_select(VALUE self, VALUE duration) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event events[EPOLL_MAX_EVENTS];
	
	int count = epoll_wait(data->descriptor, events, EPOLL_MAX_EVENTS, make_timeout(duration));
	
	if (count == -1) {
		rb_sys_fail("epoll_wait");
	}
	
	for (int i = 0; i < count; i += 1) {
		VALUE fiber = (VALUE)events[i].data.ptr;
		VALUE result = INT2NUM(events[i].events);
		
		// fprintf(stderr, "-> fiber=%p descriptor=%d\n", (void*)fiber, events[i].data.fd);
		
		rb_funcall(fiber, id_transfer, 1, result);
	}
	
	return INT2NUM(count);
}

VALUE Event_Backend_EPoll_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	pid_t pidv = NUM2PIDT(pid);
	int options = NUM2INT(flags);
	int state = 0;

	if (flags & WNOHANG > 0) {
		// WNOHANG is nonblock by default.
		return PIDT2NUM(waitpid(pidv, &state, options));
	}

	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event event = {0};
	
	int descriptor = pidfd_open(pidv, 0);
	int duplicate = -1;
	
	event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
	event.data.ptr = (void*)fiber;

	// A better approach is to batch all changes:
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1 && errno == EEXIST) {
		// The file descriptor was already inserted into epoll.
		duplicate = descriptor = dup(descriptor);
		
		rb_update_max_fd(duplicate);
		
		if (descriptor == -1)
			rb_sys_fail("dup");
		
		result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	}
	
	if (result == -1) {
		rb_sys_fail("epoll_ctl");
	}
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.descriptor = descriptor,
		.duplicate = duplicate
	};
	
	rb_ensure(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_ensure, (VALUE)&io_wait_arguments);
	return PIDT2NUM(waitpid(pidv, &state, options));
}

void Init_Event_Backend_EPoll(VALUE Event_Backend) {
	id_fileno = rb_intern("fileno");
	id_transfer = rb_intern("transfer");
	
	Event_Backend_EPoll = rb_define_class_under(Event_Backend, "EPoll", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_EPoll, Event_Backend_EPoll_allocate);
	rb_define_method(Event_Backend_EPoll, "initialize", Event_Backend_EPoll_initialize, 1);
	
	rb_define_method(Event_Backend_EPoll, "io_wait", Event_Backend_EPoll_io_wait, 3);
	rb_define_method(Event_Backend_EPoll, "select", Event_Backend_EPoll_select, 1);
	rb_define_method(Event_Backend_EPoll, "process_wait", Event_Backend_EPoll_process_wait, 3);
}
