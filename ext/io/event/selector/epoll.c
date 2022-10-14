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

#include <sys/epoll.h>
#include <time.h>
#include <errno.h>

#include "pidfd.c"
#include "../interrupt.h"

enum {
	DEBUG = 0,
};

static VALUE IO_Event_Selector_EPoll = Qnil;

enum {EPOLL_MAX_EVENTS = 64};

struct IO_Event_Selector_EPoll {
	struct IO_Event_Selector backend;
	int descriptor;
	int blocked;
	struct IO_Event_Interrupt interrupt;
};

void IO_Event_Selector_EPoll_Type_mark(void *_data)
{
	struct IO_Event_Selector_EPoll *data = _data;
	IO_Event_Selector_mark(&data->backend);
}

static
void close_internal(struct IO_Event_Selector_EPoll *data) {
	if (data->descriptor >= 0) {
		close(data->descriptor);
		data->descriptor = -1;
		
		IO_Event_Interrupt_close(&data->interrupt);
	}
}

void IO_Event_Selector_EPoll_Type_free(void *_data)
{
	struct IO_Event_Selector_EPoll *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t IO_Event_Selector_EPoll_Type_size(const void *data)
{
	return sizeof(struct IO_Event_Selector_EPoll);
}

static const rb_data_type_t IO_Event_Selector_EPoll_Type = {
	.wrap_struct_name = "IO_Event::Backend::EPoll",
	.function = {
		.dmark = IO_Event_Selector_EPoll_Type_mark,
		.dfree = IO_Event_Selector_EPoll_Type_free,
		.dsize = IO_Event_Selector_EPoll_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE IO_Event_Selector_EPoll_allocate(VALUE self) {
	struct IO_Event_Selector_EPoll *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, Qnil);
	data->descriptor = -1;
	
	return instance;
}

void IO_Event_Interrupt_add(struct IO_Event_Interrupt *interrupt, struct IO_Event_Selector_EPoll *data) {
	int descriptor = IO_Event_Interrupt_descriptor(interrupt);
	
	struct epoll_event event = {
		.events = EPOLLIN|EPOLLRDHUP,
		.data = {.ptr = NULL},
	};
	
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Interrupt_add:epoll_ctl");
	}
}

VALUE IO_Event_Selector_EPoll_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, loop);
	int result = epoll_create1(EPOLL_CLOEXEC);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Selector_EPoll_initialize:epoll_create");
	} else {
		data->descriptor = result;
		
		rb_update_max_fd(data->descriptor);
	}
	
	IO_Event_Interrupt_open(&data->interrupt);
	IO_Event_Interrupt_add(&data->interrupt, data);
	
	return self;
}

VALUE IO_Event_Selector_EPoll_loop(VALUE self) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return data->backend.loop;
}

VALUE IO_Event_Selector_EPoll_close(VALUE self) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_transfer(VALUE self)
{
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_EPoll_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return IO_Event_Selector_resume(&data->backend, argc, argv);
}

VALUE IO_Event_Selector_EPoll_yield(VALUE self)
{
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return IO_Event_Selector_yield(&data->backend);
}

VALUE IO_Event_Selector_EPoll_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	IO_Event_Selector_queue_push(&data->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return IO_Event_Selector_raise(&data->backend, argc, argv);
}

VALUE IO_Event_Selector_EPoll_ready_p(VALUE self) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct IO_Event_Selector_EPoll *data;
	pid_t pid;
	int flags;
	int descriptor;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return IO_Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	// epoll_ctl(arguments->data->descriptor, EPOLL_CTL_DEL, arguments->descriptor, NULL);
	
	close(arguments->descriptor);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = NUM2INT(flags),
	};
	
	process_wait_arguments.descriptor = pidfd_open(process_wait_arguments.pid, 0);
	
	if (process_wait_arguments.descriptor == -1) {
		rb_sys_fail("IO_Event_Selector_EPoll_process_wait:pidfd_open");
	}
	
	rb_update_max_fd(process_wait_arguments.descriptor);
	
	struct epoll_event event = {
		.events = EPOLLIN|EPOLLERR|EPOLLHUP|EPOLLONESHOT,
		.data = {.ptr = (void*)fiber},
	};
	
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, process_wait_arguments.descriptor, &event);
	
	if (result == -1) {
		close(process_wait_arguments.descriptor);
		rb_sys_fail("IO_Event_Selector_EPoll_process_wait:epoll_ctl");
	}
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

static inline
uint32_t epoll_flags_from_events(int events) {
	uint32_t flags = 0;
	
	if (events & IO_EVENT_READABLE) flags |= EPOLLIN;
	if (events & IO_EVENT_PRIORITY) flags |= EPOLLPRI;
	if (events & IO_EVENT_WRITABLE) flags |= EPOLLOUT;
	
	flags |= EPOLLHUP;
	flags |= EPOLLERR;
	
	// Immediately remove this descriptor after reading one event:
	flags |= EPOLLONESHOT;
	
	if (DEBUG) fprintf(stderr, "epoll_flags_from_events events=%d flags=%d\n", events, flags);
	
	return flags;
}

static inline
int events_from_epoll_flags(uint32_t flags) {
	int events = 0;
	
	if (DEBUG) fprintf(stderr, "events_from_epoll_flags flags=%d\n", flags);
	
	// Occasionally, (and noted specifically when dealing with child processes stdout), flags will only be POLLHUP. In this case, we arm the file descriptor for reading so that the HUP will be noted, rather than potentially ignored, since there is no dedicated event for it.
	// if (flags & (EPOLLIN)) events |= IO_EVENT_READABLE;
	if (flags & (EPOLLIN|EPOLLHUP|EPOLLERR)) events |= IO_EVENT_READABLE;
	if (flags & EPOLLPRI) events |= IO_EVENT_PRIORITY;
	if (flags & EPOLLOUT) events |= IO_EVENT_WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct IO_Event_Selector_EPoll *data;
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
	
	VALUE result = IO_Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	if (DEBUG) fprintf(stderr, "io_wait_transfer errno=%d\n", errno);
	
	// If the fiber is being cancelled, it might be resumed with nil:
	if (!RTEST(result)) {
		if (DEBUG) fprintf(stderr, "io_wait_transfer flags=false\n");
		return Qfalse;
	}
	
	if (DEBUG) fprintf(stderr, "io_wait_transfer flags=%d\n", NUM2INT(result));
	
	return INT2NUM(events_from_epoll_flags(NUM2INT(result)));
};

VALUE IO_Event_Selector_EPoll_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	struct epoll_event event = {0};
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	int duplicate = -1;
	
	event.events = epoll_flags_from_events(NUM2INT(events));
	event.data.ptr = (void*)fiber;
	
	if (DEBUG) fprintf(stderr, "<- fiber=%p descriptor=%d\n", (void*)fiber, descriptor);
	
	// A better approach is to batch all changes:
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	
	if (result == -1 && errno == EEXIST) {
		// The file descriptor was already inserted into epoll.
		duplicate = dup(descriptor);
		
		if (duplicate == -1) {
			rb_sys_fail("IO_Event_Selector_EPoll_io_wait:dup");
		}
		
		descriptor = duplicate;
		
		rb_update_max_fd(descriptor);
		
		result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);
	}
	
	if (result == -1) {
		// If we duplicated the file descriptor, ensure it's closed:
		if (duplicate >= 0) {
			close(duplicate);
		}
		
		if (errno == EPERM) {
			IO_Event_Selector_queue_push(&data->backend, fiber);
			IO_Event_Selector_yield(&data->backend);
			return events;
		}
		
		rb_sys_fail("IO_Event_Selector_EPoll_io_wait:epoll_ctl");
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
	size_t offset;
};

static
VALUE io_read_loop(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	void *base;
	size_t size;
	rb_io_buffer_get_bytes_for_writing(arguments->buffer, &base, &size);
	
	size_t length = arguments->length;
	size_t offset = arguments->offset;
	
	while (true) {
		size_t maximum_size = size - offset;
		ssize_t result = read(arguments->descriptor, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	return rb_fiber_scheduler_io_result(offset, 0);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE IO_Event_Selector_EPoll_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t offset = NUM2SIZET(_offset);
	size_t length = NUM2SIZET(_length);
	
	struct io_read_arguments io_read_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_read_loop, (VALUE)&io_read_arguments, io_read_ensure, (VALUE)&io_read_arguments);
}

VALUE IO_Event_Selector_EPoll_io_read_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_EPoll_io_read(self, argv[0], argv[1], argv[2], argv[3], _offset);
}

struct io_write_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int descriptor;
	
	VALUE buffer;
	size_t length;
	size_t offset;
};

static
VALUE io_write_loop(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	const void *base;
	size_t size;
	rb_io_buffer_get_bytes_for_reading(arguments->buffer, &base, &size);
	
	size_t length = arguments->length;
	size_t offset = arguments->offset;
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	while (true) {
		size_t maximum_size = size - offset;
		ssize_t result = write(arguments->descriptor, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_EPoll_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_WRITABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	return rb_fiber_scheduler_io_result(offset, 0);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE IO_Event_Selector_EPoll_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	
	struct io_write_arguments io_write_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(descriptor),
		.descriptor = descriptor,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_write_loop, (VALUE)&io_write_arguments, io_write_ensure, (VALUE)&io_write_arguments);
}

VALUE IO_Event_Selector_EPoll_io_write_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_EPoll_io_write(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	struct IO_Event_Selector_EPoll *data;
	
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
	arguments->data->blocked = 1;
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->data->blocked = 0;
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_without_gvl:epoll_wait");
		} else {
			arguments->count = 0;
		}
	}
}

static
void select_internal_with_gvl(struct select_arguments *arguments) {
	select_internal((void *)arguments);
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_with_gvl:epoll_wait");
		} else {
			arguments->count = 0;
		}
	}
}

VALUE IO_Event_Selector_EPoll_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	int ready = IO_Event_Selector_queue_flush(&data->backend);
	
	struct select_arguments arguments = {
		.data = data,
		.timeout = 0
	};
	
	// Process any currently pending events:
	select_internal_with_gvl(&arguments);
	
	// If we:
	// 1. Didn't process any ready fibers, and
	// 2. Didn't process any events from non-blocking select (above), and
	// 3. There are no items in the ready list,
	// then we can perform a blocking select.
	if (!ready && !arguments.count && !data->backend.ready) {
		arguments.timeout = make_timeout(duration);
		
		if (arguments.timeout != 0) {
			// Wait for events to occur
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		const struct epoll_event *event = &arguments.events[i];
		if (DEBUG) fprintf(stderr, "-> ptr=%p events=%d\n", event->data.ptr, event->events);
		
		if (event->data.ptr) {
			VALUE fiber = (VALUE)event->data.ptr;
			VALUE result = INT2NUM(event->events);
			
			IO_Event_Selector_fiber_transfer(fiber, 1, &result);
		} else {
			IO_Event_Interrupt_clear(&data->interrupt);
		}
	}
	
	return INT2NUM(arguments.count);
}

VALUE IO_Event_Selector_EPoll_wakeup(VALUE self) {
	struct IO_Event_Selector_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_EPoll, &IO_Event_Selector_EPoll_Type, data);
	
	// If we are blocking, we can schedule a nop event to wake up the selector:
	if (data->blocked) {
		IO_Event_Interrupt_signal(&data->interrupt);
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_EPoll(VALUE IO_Event_Selector) {
	IO_Event_Selector_EPoll = rb_define_class_under(IO_Event_Selector, "EPoll", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_EPoll);
	
	rb_define_alloc_func(IO_Event_Selector_EPoll, IO_Event_Selector_EPoll_allocate);
	rb_define_method(IO_Event_Selector_EPoll, "initialize", IO_Event_Selector_EPoll_initialize, 1);
	
	rb_define_method(IO_Event_Selector_EPoll, "loop", IO_Event_Selector_EPoll_loop, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "transfer", IO_Event_Selector_EPoll_transfer, 0);
	rb_define_method(IO_Event_Selector_EPoll, "resume", IO_Event_Selector_EPoll_resume, -1);
	rb_define_method(IO_Event_Selector_EPoll, "yield", IO_Event_Selector_EPoll_yield, 0);
	rb_define_method(IO_Event_Selector_EPoll, "push", IO_Event_Selector_EPoll_push, 1);
	rb_define_method(IO_Event_Selector_EPoll, "raise", IO_Event_Selector_EPoll_raise, -1);
	
	rb_define_method(IO_Event_Selector_EPoll, "ready?", IO_Event_Selector_EPoll_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "select", IO_Event_Selector_EPoll_select, 1);
	rb_define_method(IO_Event_Selector_EPoll, "wakeup", IO_Event_Selector_EPoll_wakeup, 0);
	rb_define_method(IO_Event_Selector_EPoll, "close", IO_Event_Selector_EPoll_close, 0);
	
	rb_define_method(IO_Event_Selector_EPoll, "io_wait", IO_Event_Selector_EPoll_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_EPoll, "io_read", IO_Event_Selector_EPoll_io_read_compatible, -1);
	rb_define_method(IO_Event_Selector_EPoll, "io_write", IO_Event_Selector_EPoll_io_write_compatible, -1);
#endif
	
	// Once compatibility isn't a concern, we can do this:
	// rb_define_method(IO_Event_Selector_EPoll, "io_read", IO_Event_Selector_EPoll_io_read, 5);
	// rb_define_method(IO_Event_Selector_EPoll, "io_write", IO_Event_Selector_EPoll_io_write, 5);
	
	rb_define_method(IO_Event_Selector_EPoll, "process_wait", IO_Event_Selector_EPoll_process_wait, 3);
}
