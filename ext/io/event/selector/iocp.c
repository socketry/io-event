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

#include "iocp.h"
#include "selector.h"
#include "list.h"
#include "array.h"

#include <windows.h>
#include <time.h>
#include <errno.h>

#include "../interrupt.h"

enum {
	DEBUG = 0,
};

static VALUE IO_Event_Selector_IOCP = Qnil;

enum {IOCP_MAX_THREADS = 8};

// This represents an actual fiber waiting for a specific event.
struct IO_Event_Selector_IOCP_Waiting
{
	struct IO_Event_List list;
	
	// The events the fiber is waiting for.
	enum IO_Event events;
	
	// The events that are currently ready.
	enum IO_Event ready;
	
	// The fiber value itself.
	VALUE fiber;
};

struct IO_Event_Selector_IOCP
{
	struct IO_Event_Selector backend;
	HANDLE handle;
	int blocked;
	
	struct timespec idle_duration;
	
	struct IO_Event_Interrupt interrupt;
	struct IO_Event_Array handles;
};

// This represents zero or more fibers waiting for a specific handle.
struct IO_Event_Selector_IOCP_Descriptor
{
	struct IO_Event_List list;
	
	// The last IO object that was used to register events.
	VALUE io;
	
	// The union of all events we are waiting for:
	enum IO_Event waiting_events;
	
	// The union of events we are registered for:
	enum IO_Event registered_events;
};

static
void IO_Event_Selector_IOCP_Waiting_mark(struct IO_Event_List *_waiting)
{
	struct IO_Event_Selector_IOCP_Waiting *waiting = (void*)_waiting;
	
	if (waiting->fiber) {
		rb_gc_mark_movable(waiting->fiber);
	}
}

static
void IO_Event_Selector_IOCP_Descriptor_mark(void *_handle)
{
	struct IO_Event_Selector_IOCP_Descriptor *handle = _handle;
	
	IO_Event_List_immutable_each(&handle->list, IO_Event_Selector_IOCP_Waiting_mark);
	
	if (handle->io) {
		rb_gc_mark_movable(handle->io);
	}
}

static
void IO_Event_Selector_IOCP_Type_mark(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	
	IO_Event_Selector_mark(&selector->backend);
	IO_Event_Array_each(&selector->handles, IO_Event_Selector_IOCP_Descriptor_mark);
}

static
void IO_Event_Selector_IOCP_Waiting_compact(struct IO_Event_List *_waiting)
{
	struct IO_Event_Selector_IOCP_Waiting *waiting = (void*)_waiting;
	
	if (waiting->fiber) {
		waiting->fiber = rb_gc_location(waiting->fiber);
	}
}

static
void IO_Event_Selector_IOCP_Descriptor_compact(void *_handle)
{
	struct IO_Event_Selector_IOCP_Descriptor *handle = _handle;
	
	IO_Event_List_immutable_each(&handle->list, IO_Event_Selector_IOCP_Waiting_compact);
	
	if (handle->io) {
		handle->io = rb_gc_location(handle->io);
	}
}

static
void IO_Event_Selector_IOCP_Type_compact(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	
	IO_Event_Selector_compact(&selector->backend);
	IO_Event_Array_each(&selector->handles, IO_Event_Selector_IOCP_Descriptor_compact);
}

static
void close_internal(struct IO_Event_Selector_IOCP *selector)
{
	if (selector->handle) {
		CloseHandle(selector->handle);
		selector->handle = NULL;
		
		IO_Event_Interrupt_close(&selector->interrupt);
	}
}

static
void IO_Event_Selector_IOCP_Type_free(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	
	close_internal(selector);
	
	IO_Event_Array_free(&selector->handles);
	
	free(selector);
}

static
size_t IO_Event_Selector_IOCP_Type_size(const void *_selector)
{
	const struct IO_Event_Selector_IOCP *selector = _selector;
	
	return sizeof(struct IO_Event_Selector_IOCP)
		+ IO_Event_Array_memory_size(&selector->handles)
	;
}

static const rb_data_type_t IO_Event_Selector_IOCP_Type = {
	.wrap_struct_name = "IO_Event::Backend::IOCP",
	.function = {
		.dmark = IO_Event_Selector_IOCP_Type_mark,
		.dcompact = IO_Event_Selector_IOCP_Type_compact,
		.dfree = IO_Event_Selector_IOCP_Type_free,
		.dsize = IO_Event_Selector_IOCP_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

inline static
struct IO_Event_Selector_IOCP_Descriptor * IO_Event_Selector_IOCP_Descriptor_lookup(struct IO_Event_Selector_IOCP *selector, int handle)
{
	struct IO_Event_Selector_IOCP_Descriptor *iocp_handle = IO_Event_Array_lookup(&selector->handles, handle);
	
	if (!iocp_handle) {
		rb_sys_fail("IO_Event_Selector_IOCP_Descriptor_lookup:IO_Event_Array_lookup");
	}
	
	return iocp_handle;
}

inline static
int IO_Event_Selector_IOCP_Descriptor_update(struct IO_Event_Selector_IOCP *selector, VALUE io, int handle, struct IO_Event_Selector_IOCP_Descriptor *iocp_handle)
{
	if (iocp_handle->io == io) {
		if (iocp_handle->registered_events == iocp_handle->waiting_events) {
			// All the events we are interested in are already registered.
			return 0;
		}
	} else {
		// The IO has changed, we need to reset the state:
		iocp_handle->registered_events = 0;
		iocp_handle->io = io;
	}
	
	if (iocp_handle->waiting_events == 0) {
		if (iocp_handle->registered_events) {
			// We are no longer interested in any events.
			iocp_ctl(selector->handle, IOCP_CTL_DEL, handle, NULL);
			iocp_handle->registered_events = 0;
		}
		
		iocp_handle->io = 0;
		
		return 0;
	}
	
	// We need to register for additional events:
	struct iocp_event event = {
		.events = iocp_flags_from_events(iocp_handle->waiting_events),
		.data = {.fd = handle},
	};
	
	int operation;
	
	if (iocp_handle->registered_events) {
		operation = IOCP_CTL_MOD;
	} else {
		operation = IOCP_CTL_ADD;
	}
	
	int result = iocp_ctl(selector->handle, operation, handle, &event);
	if (result == -1) {
		if (errno == ENOENT) {
			result = iocp_ctl(selector->handle, IOCP_CTL_ADD, handle, &event);
		} else if (errno == EEXIST) {
			result = iocp_ctl(selector->handle, IOCP_CTL_MOD, handle, &event);
		}
		
		if (result == -1) {
			return -1;
		}
	}
	
	iocp_handle->registered_events = iocp_handle->waiting_events;
	
	return 1;
}

inline static
int IO_Event_Selector_IOCP_Waiting_register(struct IO_Event_Selector_IOCP *selector, VALUE io, int handle, struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	struct IO_Event_Selector_IOCP_Descriptor *iocp_handle = IO_Event_Selector_IOCP_Descriptor_lookup(selector, handle);
	
	// We are waiting for these events:
	iocp_handle->waiting_events |= waiting->events;
	
	int result = IO_Event_Selector_IOCP_Descriptor_update(selector, io, handle, iocp_handle);
	if (result == -1) return -1;
	
	IO_Event_List_prepend(&iocp_handle->list, &waiting->list);
	
	return result;
}

inline static
void IO_Event_Selector_IOCP_Waiting_cancel(struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	IO_Event_List_pop(&waiting->list);
	waiting->fiber = 0;
}

void IO_Event_Selector_IOCP_Descriptor_initialize(void *element)
{
	struct IO_Event_Selector_IOCP_Descriptor *iocp_handle = element;
	IO_Event_List_initialize(&iocp_handle->list);
	iocp_handle->io = 0;
	iocp_handle->waiting_events = 0;
	iocp_handle->registered_events = 0;
}

void IO_Event_Selector_IOCP_Descriptor_free(void *element)
{
	struct IO_Event_Selector_IOCP_Descriptor *iocp_handle = element;
	
	IO_Event_List_free(&iocp_handle->list);
}

VALUE IO_Event_Selector_IOCP_allocate(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, Qnil);
	selector->handle = NULL;
	selector->blocked = 0;
	
	selector->descriptors.element_initialize = IO_Event_Selector_IOCP_Descriptor_initialize;
	selector->descriptors.element_free = IO_Event_Selector_IOCP_Descriptor_free;
	IO_Event_Array_allocate(&selector->descriptors, 1024, sizeof(struct IO_Event_Selector_IOCP_Descriptor));
	
	return instance;
}

void IO_Event_Interrupt_add(struct IO_Event_Interrupt *interrupt, struct IO_Event_Selector_IOCP *selector) {
	int handle = IO_Event_Interrupt_handle(interrupt);
	
	struct iocp_event event = {
		.events = IOCPIN|IOCPRDHUP,
		.data = {.fd = -1},
	};
	
	int result = iocp_ctl(selector->handle, IOCP_CTL_ADD, handle, &event);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Interrupt_add:iocp_ctl");
	}
}

VALUE IO_Event_Selector_IOCP_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	IO_Event_Selector_initialize(&selector->backend, loop);
	int result = iocp_create1(IOCP_CLOEXEC);
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Selector_IOCP_initialize:iocp_create");
	} else {
		selector->handle = result;
		
		rb_update_max_fd(selector->handle);
	}
	
	IO_Event_Interrupt_open(&selector->interrupt);
	IO_Event_Interrupt_add(&selector->interrupt, selector);
	
	return self;
}

VALUE IO_Event_Selector_IOCP_loop(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return selector->backend.loop;
}

VALUE IO_Event_Selector_IOCP_idle_duration(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	double duration = selector->idle_duration.tv_sec + (selector->idle_duration.tv_nsec / 1000000000.0);
	
	return DBL2NUM(duration);
}

VALUE IO_Event_Selector_IOCP_close(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	close_internal(selector);
	
	return Qnil;
}

VALUE IO_Event_Selector_IOCP_transfer(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return IO_Event_Selector_fiber_transfer(selector->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_IOCP_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return IO_Event_Selector_resume(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_IOCP_yield(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return IO_Event_Selector_yield(&selector->backend);
}

VALUE IO_Event_Selector_IOCP_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	IO_Event_Selector_queue_push(&selector->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_IOCP_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return IO_Event_Selector_raise(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_IOCP_ready_p(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	return selector->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct IO_Event_Selector_IOCP *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
	int pid;
	int flags;
	int handle;
};

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->selector->backend.loop, 0, NULL);
	
	if (arguments->waiting->ready) {
		return IO_Event_Selector_process_status_wait(arguments->pid, arguments->flags);
	} else {
		return Qfalse;
	}
}

static
VALUE process_wait_ensure(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	close(arguments->handle);
	
	IO_Event_Selector_IOCP_Waiting_cancel(arguments->waiting);
	
	return Qnil;
}

struct IO_Event_List_Type IO_Event_Selector_IOCP_process_wait_list_type = {};

VALUE IO_Event_Selector_IOCP_process_wait(VALUE self, VALUE fiber, VALUE _pid, VALUE _flags) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	pid_t pid = NUM2PIDT(_pid);
	int flags = NUM2INT(_flags);
	
	int handle = pidfd_open(pid, 0);
	
	if (handle == -1) {
		rb_sys_fail("IO_Event_Selector_IOCP_process_wait:pidfd_open");
	}
	
	rb_update_max_fd(handle);
	
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.list = {.type = &IO_Event_Selector_IOCP_process_wait_list_type},
		.fiber = fiber,
		.events = IO_EVENT_READABLE,
	};
	
	int result = IO_Event_Selector_IOCP_Waiting_register(selector, 0, handle, &waiting);
	
	if (result == -1) {
		close(handle);
		rb_sys_fail("IO_Event_Selector_IOCP_process_wait:IO_Event_Selector_IOCP_Waiting_register");
	}
	
	struct process_wait_arguments process_wait_arguments = {
		.selector = selector,
		.pid = pid,
		.flags = flags,
		.handle = handle,
		.waiting = &waiting,
	};
	
	return rb_ensure(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_ensure, (VALUE)&process_wait_arguments);
}

struct io_wait_arguments {
	struct IO_Event_Selector_IOCP *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
};

static
VALUE io_wait_ensure(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	IO_Event_Selector_IOCP_Waiting_cancel(arguments->waiting);
	
	return Qnil;
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->selector->backend.loop, 0, NULL);
	
	if (arguments->waiting->ready) {
		return RB_INT2NUM(arguments->waiting->ready);
	} else {
		return Qfalse;
	}
};

struct IO_Event_List_Type IO_Event_Selector_IOCP_io_wait_list_type = {};

VALUE IO_Event_Selector_IOCP_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	int handle = IO_Event_Selector_io_handle(io); 
	
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.list = {.type = &IO_Event_Selector_IOCP_io_wait_list_type},
		.fiber = fiber,
		.events = RB_NUM2INT(events),
	};
	
	int result = IO_Event_Selector_IOCP_Waiting_register(selector, io, handle, &waiting);
	
	if (result == -1) {
		if (errno == EPERM) {
			IO_Event_Selector_queue_push(&selector->backend, fiber);
			IO_Event_Selector_yield(&selector->backend);
			return events;
		}
		
		rb_sys_fail("IO_Event_Selector_IOCP_io_wait:IO_Event_Selector_IOCP_Waiting_register");
	}
	
	struct io_wait_arguments io_wait_arguments = {
		.selector = selector,
		.waiting = &waiting,
	};
	
	return rb_ensure(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_ensure, (VALUE)&io_wait_arguments);
}

#ifdef HAVE_RUBY_IO_BUFFER_H

struct io_read_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int handle;
	
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
	size_t total = 0;
	
	size_t maximum_size = size - offset;
	while (maximum_size) {
		ssize_t result = read(arguments->handle, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			total += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_IOCP_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
		
		maximum_size = size - offset;
	}
	
	return rb_fiber_scheduler_io_result(total, 0);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->handle, arguments->flags);
	
	return Qnil;
}

VALUE IO_Event_Selector_IOCP_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	int handle = IO_Event_Selector_io_handle(io);
	
	size_t offset = NUM2SIZET(_offset);
	size_t length = NUM2SIZET(_length);
	
	struct io_read_arguments io_read_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(handle),
		.handle = handle,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_read_loop, (VALUE)&io_read_arguments, io_read_ensure, (VALUE)&io_read_arguments);
}

VALUE IO_Event_Selector_IOCP_io_read_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_IOCP_io_read(self, argv[0], argv[1], argv[2], argv[3], _offset);
}

struct io_write_arguments {
	VALUE self;
	VALUE fiber;
	VALUE io;
	
	int flags;
	
	int handle;
	
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
	size_t total = 0;
	
	if (length > size) {
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	}
	
	size_t maximum_size = size - offset;
	while (maximum_size) {
		ssize_t result = write(arguments->handle, (char*)base+offset, maximum_size);
		
		if (result > 0) {
			total += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			IO_Event_Selector_IOCP_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_WRITABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, errno);
		}
		
		maximum_size = size - offset;
	}
	
	return rb_fiber_scheduler_io_result(total, 0);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->handle, arguments->flags);
	
	return Qnil;
};

VALUE IO_Event_Selector_IOCP_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	int handle = IO_Event_Selector_io_handle(io);
	
	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	
	struct io_write_arguments io_write_arguments = {
		.self = self,
		.fiber = fiber,
		.io = io,
		
		.flags = IO_Event_Selector_nonblock_set(handle),
		.handle = handle,
		.buffer = buffer,
		.length = length,
		.offset = offset,
	};
	
	return rb_ensure(io_write_loop, (VALUE)&io_write_arguments, io_write_ensure, (VALUE)&io_write_arguments);
}

VALUE IO_Event_Selector_IOCP_io_write_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_IOCP_io_write(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	struct IO_Event_Selector_IOCP *selector;
	
	int count;
	struct iocp_event events[IOCP_MAX_EVENTS];
	
	struct timespec * timeout;
	struct timespec storage;
	
	struct IO_Event_List saved;
};

static int make_timeout_ms(struct timespec * timeout) {
	if (timeout == NULL) {
		return -1;
	}
	
	if (timeout_nonblocking(timeout)) {
		return 0;
	}
	
	return (timeout->tv_sec * 1000) + (timeout->tv_nsec / 1000000);
}

static
int enosys_error(int result) {
	if (result == -1) {
		return errno == ENOSYS;
	}
	
	return 0;
}

static
void * select_internal(void *_arguments) {
	struct select_arguments * arguments = (struct select_arguments *)_arguments;
	
	arguments->count = iocp_pwait2(arguments->selector->handle, arguments->events, IOCP_MAX_EVENTS, arguments->timeout, NULL);
	
	// Comment out the above line and enable the below lines to test ENOSYS code path.
	// arguments->count = -1;
	// errno = ENOSYS;
	
	if (!enosys_error(arguments->count)) {
		return NULL;
	}
	else {
		// Fall through and execute iocp_wait fallback.
	}
#endif
	
	arguments->count = iocp_wait(arguments->selector->handle, arguments->events, IOCP_MAX_EVENTS, make_timeout_ms(arguments->timeout));
	
	return NULL;
}

static
void select_internal_without_gvl(struct select_arguments *arguments) {
	arguments->selector->blocked = 1;
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->selector->blocked = 0;
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_without_gvl:iocp_wait");
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
			rb_sys_fail("select_internal_with_gvl:iocp_wait");
		} else {
			arguments->count = 0;
		}
	}
}

static
int IO_Event_Selector_IOCP_handle(struct IO_Event_Selector_IOCP *selector, const struct iocp_event *event, struct IO_Event_List *saved)
{
	int handle = event->data.fd;
	
	// This is the mask of all events that occured for the given handle:
	enum IO_Event ready_events = events_from_iocp_flags(event->events);
	
	struct IO_Event_Selector_IOCP_Descriptor *iocp_handle = IO_Event_Selector_IOCP_Descriptor_lookup(selector, handle);
	struct IO_Event_List *list = &iocp_handle->list;
	struct IO_Event_List *node = list->tail;
	
	// Reset the events back to 0 so that we can re-arm if necessary:
	iocp_handle->waiting_events = 0;
	
	if (DEBUG) fprintf(stderr, "IO_Event_Selector_IOCP_handle: handle=%d, ready_events=%d iocp_handle=%p\n", handle, ready_events, iocp_handle);
	
	// It's possible (but unlikely) that the address of list will changing during iteration.
	while (node != list) {
		if (DEBUG) fprintf(stderr, "IO_Event_Selector_IOCP_handle: node=%p list=%p type=%p\n", node, list, node->type);
		
		struct IO_Event_Selector_IOCP_Waiting *waiting = (struct IO_Event_Selector_IOCP_Waiting *)node;
		
		// Compute the intersection of the events we are waiting for and the events that occured:
		enum IO_Event matching_events = waiting->events & ready_events;
		
		if (DEBUG) fprintf(stderr, "IO_Event_Selector_IOCP_handle: handle=%d, ready_events=%d, waiting_events=%d, matching_events=%d\n", handle, ready_events, waiting->events, matching_events);
		
		if (matching_events) {
			IO_Event_List_append(node, saved);
			
			// Resume the fiber:
			waiting->ready = matching_events;
			IO_Event_Selector_fiber_transfer(waiting->fiber, 0, NULL);
			
			node = saved->tail;
			IO_Event_List_pop(saved);
		} else {
			// We are still waiting for the events:
			iocp_handle->waiting_events |= waiting->events;
			node = node->tail;
		}
	}
	
	return IO_Event_Selector_IOCP_Descriptor_update(selector, iocp_handle->io, handle, iocp_handle);
}

static
VALUE select_handle_events(VALUE _arguments)
{
	struct select_arguments *arguments = (struct select_arguments *)_arguments;
	struct IO_Event_Selector_IOCP *selector = arguments->selector;
	
	for (int i = 0; i < arguments->count; i += 1) {
		const struct iocp_event *event = &arguments->events[i];
		if (DEBUG) fprintf(stderr, "-> fd=%d events=%d\n", event->data.fd, event->events);
		
		if (event->data.fd >= 0) {
			IO_Event_Selector_IOCP_handle(selector, event, &arguments->saved);
		} else {
			IO_Event_Interrupt_clear(&selector->interrupt);
		}
	}
	
	return INT2NUM(arguments->count);
}

static
VALUE select_handle_events_ensure(VALUE _arguments)
{
	struct select_arguments *arguments = (struct select_arguments *)_arguments;
	
	IO_Event_List_free(&arguments->saved);
	
	return Qnil;
}

// TODO This function is not re-entrant and we should document and assert as such.
VALUE IO_Event_Selector_IOCP_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	selector->idle_duration.tv_sec = 0;
	selector->idle_duration.tv_nsec = 0;
	
	int ready = IO_Event_Selector_queue_flush(&selector->backend);
	
	struct select_arguments arguments = {
		.selector = selector,
		.storage = {
			.tv_sec = 0,
			.tv_nsec = 0
		},
		.saved = {},
	};

	arguments.timeout = &arguments.storage;

	// Process any currently pending events:
	select_internal_with_gvl(&arguments);
	
	// If we:
	// 1. Didn't process any ready fibers, and
	// 2. Didn't process any events from non-blocking select (above), and
	// 3. There are no items in the ready list,
	// then we can perform a blocking select.
	if (!ready && !arguments.count && !selector->backend.ready) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			struct timespec start_time;
			IO_Event_Selector_current_time(&start_time);
			
			// Wait for events to occur:
			select_internal_without_gvl(&arguments);
			
			struct timespec end_time;
			IO_Event_Selector_current_time(&end_time);
			IO_Event_Selector_elapsed_time(&start_time, &end_time, &selector->idle_duration);
		}
	}
	
	if (arguments.count) {
		return rb_ensure(select_handle_events, (VALUE)&arguments, select_handle_events_ensure, (VALUE)&arguments);
	} else {
		return RB_INT2NUM(0);
	}
}

VALUE IO_Event_Selector_IOCP_wakeup(VALUE self) {
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP, &IO_Event_Selector_IOCP_Type, selector);
	
	// If we are blocking, we can schedule a nop event to wake up the selector:
	if (selector->blocked) {
		IO_Event_Interrupt_signal(&selector->interrupt);
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_IOCP(VALUE IO_Event_Selector) {
	IO_Event_Selector_IOCP = rb_define_class_under(IO_Event_Selector, "IOCP", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_IOCP);
	
	rb_define_alloc_func(IO_Event_Selector_IOCP, IO_Event_Selector_IOCP_allocate);
	rb_define_method(IO_Event_Selector_IOCP, "initialize", IO_Event_Selector_IOCP_initialize, 1);
	
	rb_define_method(IO_Event_Selector_IOCP, "loop", IO_Event_Selector_IOCP_loop, 0);
	rb_define_method(IO_Event_Selector_IOCP, "idle_duration", IO_Event_Selector_IOCP_idle_duration, 0);
	
	rb_define_method(IO_Event_Selector_IOCP, "transfer", IO_Event_Selector_IOCP_transfer, 0);
	rb_define_method(IO_Event_Selector_IOCP, "resume", IO_Event_Selector_IOCP_resume, -1);
	rb_define_method(IO_Event_Selector_IOCP, "yield", IO_Event_Selector_IOCP_yield, 0);
	rb_define_method(IO_Event_Selector_IOCP, "push", IO_Event_Selector_IOCP_push, 1);
	rb_define_method(IO_Event_Selector_IOCP, "raise", IO_Event_Selector_IOCP_raise, -1);
	
	rb_define_method(IO_Event_Selector_IOCP, "ready?", IO_Event_Selector_IOCP_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_IOCP, "select", IO_Event_Selector_IOCP_select, 1);
	rb_define_method(IO_Event_Selector_IOCP, "wakeup", IO_Event_Selector_IOCP_wakeup, 0);
	rb_define_method(IO_Event_Selector_IOCP, "close", IO_Event_Selector_IOCP_close, 0);
	
	rb_define_method(IO_Event_Selector_IOCP, "io_wait", IO_Event_Selector_IOCP_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_IOCP, "io_read", IO_Event_Selector_IOCP_io_read_compatible, -1);
	rb_define_method(IO_Event_Selector_IOCP, "io_write", IO_Event_Selector_IOCP_io_write_compatible, -1);
#endif
	
	// Once compatibility isn't a concern, we can do this:
	// rb_define_method(IO_Event_Selector_IOCP, "io_read", IO_Event_Selector_IOCP_io_read, 5);
	// rb_define_method(IO_Event_Selector_IOCP, "io_write", IO_Event_Selector_IOCP_io_write, 5);
	
	rb_define_method(IO_Event_Selector_IOCP, "process_wait", IO_Event_Selector_IOCP_process_wait, 3);
}
