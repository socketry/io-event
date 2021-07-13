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

#include "pidfd.c"

static VALUE Event_Backend_EPoll = Qnil;

enum {EPOLL_MAX_EVENTS = 64};

struct Event_Backend_EPoll {
	struct Event_Backend backend;
	int descriptor;
};

void Event_Backend_EPoll_Type_mark(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	Event_Backend_mark(&data->backend);
}

static
void close_internal(struct Event_Backend_EPoll *data) {
	if (data->descriptor >= 0) {
		close(data->descriptor);
		data->descriptor = -1;
	}
}

void Event_Backend_EPoll_Type_free(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	
	close_internal(data);
	
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
	
	Event_Backend_initialize(&data->backend, Qnil);
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Backend_EPoll_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	Event_Backend_initialize(&data->backend, loop);
	int result = epoll_create1(EPOLL_CLOEXEC);
	
	if (result == -1) {
		rb_sys_fail("epoll_create");
	} else {
		data->descriptor = result;
		
		rb_update_max_fd(data->descriptor);
	}
	
	return self;
}

VALUE Event_Backend_EPoll_close(VALUE self) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE Event_Backend_EPoll_transfer(VALUE self, VALUE fiber)
{
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	Event_Backend_wait_and_transfer(&data->backend, fiber);
	
	return Qnil;
}

VALUE Event_Backend_EPoll_defer(VALUE self)
{
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	Event_Backend_defer(&data->backend);
	
	return Qnil;
}

VALUE Event_Backend_EPoll_ready_p(VALUE self) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct Event_Backend_EPoll *data;
	pid_t pid;
	int flags;
	int descriptor;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	Event_Backend_fiber_transfer(arguments->data->backend.loop);
	
	return Event_Backend_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	// epoll_ctl(arguments->data->descriptor, EPOLL_CTL_DEL, arguments->descriptor, NULL);
	
	close(arguments->descriptor);
	
	return Qnil;
}

VALUE Event_Backend_EPoll_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = NUM2INT(flags),
	};
	
	process_wait_arguments.descriptor = pidfd_open(process_wait_arguments.pid, 0);
	rb_update_max_fd(process_wait_arguments.descriptor);
	
	struct epoll_event event = {
		.events = EPOLLIN|EPOLLRDHUP|EPOLLONESHOT,
		.data = {.ptr = (void*)fiber},
	};
	
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, process_wait_arguments.descriptor, &event);
	
	if (result == -1) {
		rb_sys_fail("epoll_ctl(process_wait)");
	}
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
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
	
	VALUE result = Event_Backend_fiber_transfer(arguments->data->backend.loop);
	
	return INT2NUM(events_from_epoll_flags(NUM2INT(result)));
};

VALUE Event_Backend_EPoll_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event event = {0};
	
	int descriptor = Event_Backend_io_descriptor(io);
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
			Event_Backend_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(READABLE));
		} else {
			rb_sys_fail("Event_Backend_EPoll_io_read");
		}
	}
	
	return SIZET2NUM(offset);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	Event_Backend_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE Event_Backend_EPoll_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	int descriptor = Event_Backend_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	
	struct io_read_arguments io_read_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = Event_Backend_nonblock_set(descriptor),
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
			Event_Backend_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(WRITABLE));
		} else {
			rb_sys_fail("Event_Backend_EPoll_io_write");
		}
	}
	
	return SIZET2NUM(offset);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	Event_Backend_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE Event_Backend_EPoll_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length) {
	int descriptor = Event_Backend_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	
	struct io_write_arguments io_write_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = Event_Backend_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
	};
	
	return rb_ensure(io_write_loop, (VALUE)&io_write_arguments, io_write_ensure, (VALUE)&io_write_arguments);
}

#endif

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

struct select_arguments {
	struct Event_Backend_EPoll *data;
	
	int count;
	struct epoll_event events[EPOLL_MAX_EVENTS];
	
	int timeout;
};

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
	arguments->count = epoll_wait(arguments->data->descriptor, arguments->events, EPOLL_MAX_EVENTS, arguments->timeout);
	
	return NULL;
}

static
void select_internal_without_gvl(struct select_arguments *arguments) {
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	
	if (arguments->count == -1) {
		rb_sys_fail("select_internal_without_gvl:epoll_wait");
	}
}

static
void select_internal_with_gvl(struct select_arguments *arguments) {
	select_internal((void *)arguments);
	
	if (arguments->count == -1) {
		rb_sys_fail("select_internal_with_gvl:epoll_wait");
	}
}

VALUE Event_Backend_EPoll_select(VALUE self, VALUE duration) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	Event_Backend_ready_pop(&data->backend);
	
	struct select_arguments arguments = {
		.data = data,
		.timeout = 0
	};
	
	select_internal_with_gvl(&arguments);
	
	if (arguments.count == 0) {
		arguments.timeout = make_timeout(duration);
		
		if (!data->backend.ready && arguments.timeout != 0) {
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		VALUE fiber = (VALUE)arguments.events[i].data.ptr;
		VALUE result = INT2NUM(arguments.events[i].events);
		
		// fprintf(stderr, "-> fiber=%p descriptor=%d\n", (void*)fiber, events[i].data.fd);
		
		Event_Backend_fiber_transfer_result(fiber, result);
	}
	
	return INT2NUM(arguments.count);
}

void Init_Event_Backend_EPoll(VALUE Event_Backend) {
	Event_Backend_EPoll = rb_define_class_under(Event_Backend, "EPoll", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_EPoll, Event_Backend_EPoll_allocate);
	rb_define_method(Event_Backend_EPoll, "initialize", Event_Backend_EPoll_initialize, 1);
	rb_define_method(Event_Backend_EPoll, "transfer", Event_Backend_EPoll_transfer, 1);
	rb_define_method(Event_Backend_EPoll, "defer", Event_Backend_EPoll_defer, 0);
	rb_define_method(Event_Backend_EPoll, "defered?", Event_Backend_EPoll_ready_p, 0);
	rb_define_method(Event_Backend_EPoll, "select", Event_Backend_EPoll_select, 1);
	rb_define_method(Event_Backend_EPoll, "close", Event_Backend_EPoll_close, 0);
	
	rb_define_method(Event_Backend_EPoll, "io_wait", Event_Backend_EPoll_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(Event_Backend_EPoll, "io_read", Event_Backend_EPoll_io_read, 4);
	rb_define_method(Event_Backend_EPoll, "io_write", Event_Backend_EPoll_io_write, 4);
#endif
	
	rb_define_method(Event_Backend_EPoll, "process_wait", Event_Backend_EPoll_process_wait, 3);
}
