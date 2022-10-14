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

enum {
	DEBUG = 0,
	DEBUG_IO_READ = 0,
	DEBUG_IO_WRITE = 0,
	DEBUG_IO_WAIT = 0
};

static VALUE IO_Event_Selector_KQueue = Qnil;

enum {KQUEUE_MAX_EVENTS = 64};

struct IO_Event_Selector_KQueue {
	struct IO_Event_Selector backend;
	int descriptor;
	
	int blocked;
};

void IO_Event_Selector_KQueue_Type_mark(void *_data)
{
	struct IO_Event_Selector_KQueue *data = _data;
	IO_Event_Selector_mark(&data->backend);
}

static
void close_internal(struct IO_Event_Selector_KQueue *data) {
	if (data->descriptor >= 0) {
		close(data->descriptor);
		data->descriptor = -1;
	}
}

void IO_Event_Selector_KQueue_Type_free(void *_data)
{
	struct IO_Event_Selector_KQueue *data = _data;
	
	close_internal(data);
	
	free(data);
}

size_t IO_Event_Selector_KQueue_Type_size(const void *data)
{
	return sizeof(struct IO_Event_Selector_KQueue);
}

static const rb_data_type_t IO_Event_Selector_KQueue_Type = {
	.wrap_struct_name = "IO_Event::Backend::KQueue",
	.function = {
		.dmark = IO_Event_Selector_KQueue_Type_mark,
		.dfree = IO_Event_Selector_KQueue_Type_free,
		.dsize = IO_Event_Selector_KQueue_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE IO_Event_Selector_KQueue_allocate(VALUE self) {
	struct IO_Event_Selector_KQueue *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, Qnil);
	data->descriptor = -1;
	data->blocked = 0;
	
	return instance;
}

VALUE IO_Event_Selector_KQueue_initialize(VALUE self, VALUE loop) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	IO_Event_Selector_initialize(&data->backend, loop);
	int result = kqueue();
	
	if (result == -1) {
		rb_sys_fail("IO_Event_Selector_KQueue_initialize:kqueue");
	} else {
		ioctl(result, FIOCLEX);
		data->descriptor = result;
		
		rb_update_max_fd(data->descriptor);
	}
	
	return self;
}

VALUE IO_Event_Selector_KQueue_loop(VALUE self) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return data->backend.loop;
}

VALUE IO_Event_Selector_KQueue_close(VALUE self) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	close_internal(data);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_transfer(VALUE self)
{
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return IO_Event_Selector_fiber_transfer(data->backend.loop, 0, NULL);
}

VALUE IO_Event_Selector_KQueue_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return IO_Event_Selector_resume(&data->backend, argc, argv);
}

VALUE IO_Event_Selector_KQueue_yield(VALUE self)
{
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return IO_Event_Selector_yield(&data->backend);
}

VALUE IO_Event_Selector_KQueue_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	IO_Event_Selector_queue_push(&data->backend, fiber);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return IO_Event_Selector_raise(&data->backend, argc, argv);
}

VALUE IO_Event_Selector_KQueue_ready_p(VALUE self) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	return data->backend.ready ? Qtrue : Qfalse;
}

struct process_wait_arguments {
	struct IO_Event_Selector_KQueue *data;
	pid_t pid;
	int flags;
};

static
int process_add_filters(int descriptor, int ident, VALUE fiber) {
	struct kevent event = {0};
	
	event.ident = ident;
	event.filter = EVFILT_PROC;
	event.flags = EV_ADD | EV_ENABLE | EV_ONESHOT | EV_UDATA_SPECIFIC;
	event.fflags = NOTE_EXIT;
	event.udata = (void*)fiber;
	
	int result = kevent(descriptor, &event, 1, NULL, 0, NULL);
	
	if (result == -1) {
		// No such process - the process has probably already terminated:
		if (errno == ESRCH) {
			return 0;
		}
		
		rb_sys_fail("process_add_filters:kevent");
	}
	
	return 1;
}

static
void process_remove_filters(int descriptor, int ident) {
	struct kevent event = {0};
	
	event.ident = ident;
	event.filter = EVFILT_PROC;
	event.flags = EV_DELETE | EV_UDATA_SPECIFIC;
	event.fflags = NOTE_EXIT;
	
	// Ignore the result.
	kevent(descriptor, &event, 1, NULL, 0, NULL);
}

static
VALUE process_wait_transfer(VALUE _arguments) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	IO_Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	return IO_Event_Selector_process_status_wait(arguments->pid);
}

static
VALUE process_wait_rescue(VALUE _arguments, VALUE exception) {
	struct process_wait_arguments *arguments = (struct process_wait_arguments *)_arguments;
	
	process_remove_filters(arguments->data->descriptor, arguments->pid);
	
	rb_exc_raise(exception);
}

VALUE IO_Event_Selector_KQueue_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	struct process_wait_arguments process_wait_arguments = {
		.data = data,
		.pid = NUM2PIDT(pid),
		.flags = RB_NUM2INT(flags),
	};
	
	VALUE result = Qnil;
	
	// This loop should not be needed but I have seen a race condition between NOTE_EXIT and `waitpid`, thus the result would be (unexpectedly) nil. So we put this in a loop to retry if the race condition shows up:
	while (NIL_P(result)) {
		int waiting = process_add_filters(data->descriptor, process_wait_arguments.pid, fiber);
	
		if (waiting) {
			result = rb_rescue(process_wait_transfer, (VALUE)&process_wait_arguments, process_wait_rescue, (VALUE)&process_wait_arguments);
		} else {
			result = IO_Event_Selector_process_status_wait(process_wait_arguments.pid);
		}
	}
	
	return result;
}

static
int io_add_filters(int descriptor, int ident, int events, VALUE fiber) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & IO_EVENT_READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT | EV_UDATA_SPECIFIC;
		kevents[count].udata = (void*)fiber;
		
// #ifdef EV_OOBAND
// 		if (events & PRIORITY) {
// 			kevents[count].flags |= EV_OOBAND;
// 		}
// #endif
		
		count++;
	}
	
	if (events & IO_EVENT_WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_ADD | EV_ENABLE | EV_ONESHOT | EV_UDATA_SPECIFIC;
		kevents[count].udata = (void*)fiber;
		count++;
	}
	
	int result = kevent(descriptor, kevents, count, NULL, 0, NULL);
	
	if (result == -1) {
		rb_sys_fail("io_add_filters:kevent");
	}
	
	return events;
}

static
void io_remove_filters(int descriptor, int ident, int events) {
	int count = 0;
	struct kevent kevents[2] = {0};
	
	if (events & IO_EVENT_READABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_READ;
		kevents[count].flags = EV_DELETE | EV_UDATA_SPECIFIC;
		
		count++;
	}
	
	if (events & IO_EVENT_WRITABLE) {
		kevents[count].ident = ident;
		kevents[count].filter = EVFILT_WRITE;
		kevents[count].flags = EV_DELETE | EV_UDATA_SPECIFIC;
		count++;
	}
	
	// Ignore the result.
	kevent(descriptor, kevents, count, NULL, 0, NULL);
}

struct io_wait_arguments {
	struct IO_Event_Selector_KQueue *data;
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
	if (filter == EVFILT_READ) return IO_EVENT_READABLE;
	if (filter == EVFILT_WRITE) return IO_EVENT_WRITABLE;
	
	return 0;
}

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	
	VALUE result = IO_Event_Selector_fiber_transfer(arguments->data->backend.loop, 0, NULL);
	
	// If the fiber is being cancelled, it might be resumed with nil:
	if (!RTEST(result)) {
		return Qfalse;
	}
	
	return INT2NUM(events_from_kqueue_filter(RB_NUM2INT(result)));
}

VALUE IO_Event_Selector_KQueue_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	struct io_wait_arguments io_wait_arguments = {
		.events = io_add_filters(data->descriptor, descriptor, RB_NUM2INT(events), fiber),
		.data = data,
		.descriptor = descriptor,
	};
	
	if (DEBUG_IO_WAIT) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait descriptor=%d\n", descriptor);
	
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
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu)\n", arguments->descriptor, length);
	
	while (true) {
		size_t maximum_size = size - offset;
		if (DEBUG_IO_READ) fprintf(stderr, "read(%d, +%ld, %ld)\n", arguments->descriptor, offset, maximum_size);
		ssize_t result = read(arguments->descriptor, (char*)base+offset, maximum_size);
		if (DEBUG_IO_READ) fprintf(stderr, "read(%d, +%ld, %ld) -> %zd\n", arguments->descriptor, offset, maximum_size, result);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			if (DEBUG_IO_READ) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait(fd=%d, length=%zu)\n", arguments->descriptor, length);
			IO_Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu) -> errno=%d\n", arguments->descriptor, length, errno);
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_read_loop(fd=%d, length=%zu) -> %zu\n", arguments->descriptor, length, offset);
	return rb_fiber_scheduler_io_result(offset, 0);
}

static
VALUE io_read_ensure(VALUE _arguments) {
	struct io_read_arguments *arguments = (struct io_read_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
}

VALUE IO_Event_Selector_KQueue_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	int descriptor = IO_Event_Selector_io_descriptor(io);
	
	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	
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

static VALUE IO_Event_Selector_KQueue_io_read_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_KQueue_io_read(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	
	if (DEBUG_IO_WRITE) fprintf(stderr, "io_write_loop(fd=%d, length=%zu)\n", arguments->descriptor, length);
	
	while (true) {
		size_t maximum_size = size - offset;
		if (DEBUG_IO_WRITE) fprintf(stderr, "write(%d, +%ld, %ld, length=%zu)\n", arguments->descriptor, offset, maximum_size, length);
		ssize_t result = write(arguments->descriptor, (char*)base+offset, maximum_size);
		if (DEBUG_IO_WRITE) fprintf(stderr, "write(%d, +%ld, %ld) -> %zd\n", arguments->descriptor, offset, maximum_size, result);
		
		if (result > 0) {
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(errno)) {
			if (DEBUG_IO_WRITE) fprintf(stderr, "IO_Event_Selector_KQueue_io_wait(fd=%d, length=%zu)\n", arguments->descriptor, length);
			IO_Event_Selector_KQueue_io_wait(arguments->self, arguments->fiber, arguments->io, RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			if (DEBUG_IO_WRITE) fprintf(stderr, "io_write_loop(fd=%d, length=%zu) -> errno=%d\n", arguments->descriptor, length, errno);
			return rb_fiber_scheduler_io_result(-1, errno);
		}
	}
	
	if (DEBUG_IO_READ) fprintf(stderr, "io_write_loop(fd=%d, length=%zu) -> %zu\n", arguments->descriptor, length, offset);
	return rb_fiber_scheduler_io_result(offset, 0);
};

static
VALUE io_write_ensure(VALUE _arguments) {
	struct io_write_arguments *arguments = (struct io_write_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->descriptor, arguments->flags);
	
	return Qnil;
};

VALUE IO_Event_Selector_KQueue_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE _length, VALUE _offset) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
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

static VALUE IO_Event_Selector_KQueue_io_write_compatible(int argc, VALUE *argv, VALUE self)
{
	rb_check_arity(argc, 4, 5);
	
	VALUE _offset = SIZET2NUM(0);
	
	if (argc == 5) {
		_offset = argv[4];
	}
	
	return IO_Event_Selector_KQueue_io_write(self, argv[0], argv[1], argv[2], argv[3], _offset);
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
	struct IO_Event_Selector_KQueue *data;
	
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
	arguments->data->blocked = 1;
	
	rb_thread_call_without_gvl(select_internal, (void *)arguments, RUBY_UBF_IO, 0);
	arguments->data->blocked = 0;
	
	if (arguments->count == -1) {
		if (errno != EINTR) {
			rb_sys_fail("select_internal_without_gvl:kevent");
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
			rb_sys_fail("select_internal_with_gvl:kevent");
		} else {
			arguments->count = 0;
		}
	}
}

VALUE IO_Event_Selector_KQueue_select(VALUE self, VALUE duration) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	int ready = IO_Event_Selector_queue_flush(&data->backend);
	
	struct select_arguments arguments = {
		.data = data,
		.count = KQUEUE_MAX_EVENTS,
		.storage = {
			.tv_sec = 0,
			.tv_nsec = 0
		}
	};
	
	arguments.timeout = &arguments.storage;
	
	// We break this implementation into two parts.
	// (1) count = kevent(..., timeout = 0)
	// (2) without gvl: kevent(..., timeout = 0) if count == 0 and timeout != 0
	// This allows us to avoid releasing and reacquiring the GVL.
	// Non-comprehensive testing shows this gives a 1.5x speedup.
	
	// First do the syscall with no timeout to get any immediately available events:
	if (DEBUG) fprintf(stderr, "\r\nselect_internal_with_gvl timeout=" PRINTF_TIMESPEC "\r\n", PRINTF_TIMESPEC_ARGS(arguments.storage));
	select_internal_with_gvl(&arguments);
	if (DEBUG) fprintf(stderr, "\r\nselect_internal_with_gvl done\r\n");
	
	// If we:
	// 1. Didn't process any ready fibers, and
	// 2. Didn't process any events from non-blocking select (above), and
	// 3. There are no items in the ready list,
	// then we can perform a blocking select.
	if (!ready && !arguments.count && !data->backend.ready) {
		arguments.timeout = make_timeout(duration, &arguments.storage);
		
		if (!timeout_nonblocking(arguments.timeout)) {
			arguments.count = KQUEUE_MAX_EVENTS;
			
			if (DEBUG) fprintf(stderr, "IO_Event_Selector_KQueue_select timeout=" PRINTF_TIMESPEC "\n", PRINTF_TIMESPEC_ARGS(arguments.storage));
			select_internal_without_gvl(&arguments);
		}
	}
	
	for (int i = 0; i < arguments.count; i += 1) {
		if (arguments.events[i].udata) {
			VALUE fiber = (VALUE)arguments.events[i].udata;
			VALUE result = INT2NUM(arguments.events[i].filter);
			
			IO_Event_Selector_fiber_transfer(fiber, 1, &result);
		}
	}
	
	return INT2NUM(arguments.count);
}

VALUE IO_Event_Selector_KQueue_wakeup(VALUE self) {
	struct IO_Event_Selector_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_KQueue, &IO_Event_Selector_KQueue_Type, data);
	
	if (data->blocked) {
		struct kevent trigger = {0};
		
		trigger.filter = EVFILT_USER;
		trigger.flags = EV_ADD | EV_CLEAR | EV_UDATA_SPECIFIC;
		trigger.fflags = NOTE_TRIGGER;
		
		int result = kevent(data->descriptor, &trigger, 1, NULL, 0, NULL);
		
		if (result == -1) {
			rb_sys_fail("IO_Event_Selector_KQueue_wakeup:kevent");
		}
		
		return Qtrue;
	}
	
	return Qfalse;
}

void Init_IO_Event_Selector_KQueue(VALUE IO_Event_Selector) {
	IO_Event_Selector_KQueue = rb_define_class_under(IO_Event_Selector, "KQueue", rb_cObject);
	rb_gc_register_mark_object(IO_Event_Selector_KQueue);
	
	rb_define_alloc_func(IO_Event_Selector_KQueue, IO_Event_Selector_KQueue_allocate);
	rb_define_method(IO_Event_Selector_KQueue, "initialize", IO_Event_Selector_KQueue_initialize, 1);
	
	rb_define_method(IO_Event_Selector_KQueue, "loop", IO_Event_Selector_KQueue_loop, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "transfer", IO_Event_Selector_KQueue_transfer, 0);
	rb_define_method(IO_Event_Selector_KQueue, "resume", IO_Event_Selector_KQueue_resume, -1);
	rb_define_method(IO_Event_Selector_KQueue, "yield", IO_Event_Selector_KQueue_yield, 0);
	rb_define_method(IO_Event_Selector_KQueue, "push", IO_Event_Selector_KQueue_push, 1);
	rb_define_method(IO_Event_Selector_KQueue, "raise", IO_Event_Selector_KQueue_raise, -1);
	
	rb_define_method(IO_Event_Selector_KQueue, "ready?", IO_Event_Selector_KQueue_ready_p, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "select", IO_Event_Selector_KQueue_select, 1);
	rb_define_method(IO_Event_Selector_KQueue, "wakeup", IO_Event_Selector_KQueue_wakeup, 0);
	rb_define_method(IO_Event_Selector_KQueue, "close", IO_Event_Selector_KQueue_close, 0);
	
	rb_define_method(IO_Event_Selector_KQueue, "io_wait", IO_Event_Selector_KQueue_io_wait, 3);
	
#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(IO_Event_Selector_KQueue, "io_read", IO_Event_Selector_KQueue_io_read_compatible, -1);
	rb_define_method(IO_Event_Selector_KQueue, "io_write", IO_Event_Selector_KQueue_io_write_compatible, -1);
#endif
	
	rb_define_method(IO_Event_Selector_KQueue, "process_wait", IO_Event_Selector_KQueue_process_wait, 3);
}
