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
#include "backend.h"

#include <liburing.h>
#include <poll.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

static VALUE Event_Backend_URing = Qnil;
static ID id_fileno, id_transfer;

static const int URING_ENTRIES = 64;
static const int URING_MAX_EVENTS = 64;

struct Event_Backend_URing {
	VALUE loop;
	struct io_uring ring;
};

void Event_Backend_URing_Type_mark(void *_data)
{
	struct Event_Backend_URing *data = _data;
	rb_gc_mark(data->loop);
}

void Event_Backend_URing_Type_free(void *_data)
{
	struct Event_Backend_URing *data = _data;
	
	if (data->ring.ring_fd >= 0) {
		io_uring_queue_exit(&data->ring);
		data->ring.ring_fd = -1;
	}
	
	free(data);
}

size_t Event_Backend_URing_Type_size(const void *data)
{
	return sizeof(struct Event_Backend_URing);
}

static const rb_data_type_t Event_Backend_URing_Type = {
	.wrap_struct_name = "Event::Backend::URing",
	.function = {
		.dmark = Event_Backend_URing_Type_mark,
		.dfree = Event_Backend_URing_Type_free,
		.dsize = Event_Backend_URing_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Backend_URing_allocate(VALUE self) {
	struct Event_Backend_URing *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	data->loop = Qnil;
	data->ring.ring_fd = -1;
	
	return instance;
}

VALUE Event_Backend_URing_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	data->loop = loop;
	
	int result = io_uring_queue_init(URING_ENTRIES, &data->ring, 0);
	
	if (result < 0) {
		rb_syserr_fail(-result, "io_uring_queue_init");
	}
	
	rb_update_max_fd(data->ring.ring_fd);
	
	return self;
}

static inline
short poll_flags_from_events(int events) {
	short flags = 0;
	
	if (events & READABLE) flags |= POLLIN;
	if (events & PRIORITY) flags |= POLLPRI;
	if (events & WRITABLE) flags |= POLLOUT;
	
	flags |= POLLERR;
	flags |= POLLHUP;
	
	return flags;
}

static inline
int events_from_poll_flags(short flags) {
	int events = 0;
	
	if (flags & POLLIN) events |= READABLE;
	if (flags & POLLPRI) events |= PRIORITY;
	if (flags & POLLOUT) events |= WRITABLE;
	
	return events;
}

struct io_wait_arguments {
	struct Event_Backend_KQueue *data;
	VALUE fiber;
	short flags;
};

static
VALUE io_wait_rescue(VALUE _arguments, VALUE exception) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Backend_URing *data = arguments->data;
	
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	io_uring_prep_poll_remove(sqe, (void*)arguments->fiber);
	io_uring_submit(&data->ring);
	
	return Qnil;
};

static
VALUE io_wait_transfer(VALUE _arguments) {
	struct io_wait_arguments *arguments = (struct io_wait_arguments *)_arguments;
	struct Event_Backend_URing *data = arguments->data;
	
	VALUE result = rb_funcall(data->loop, id_transfer, 0);
	
	// We explicitly filter the resulting events based on the requested events.
	// In some cases, poll will report events we didn't ask for.
	short flags = arguments->flags & NUM2INT(result);
	
	return INT2NUM(events_from_poll_flags(flags));
};

VALUE Event_Backend_URing_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	short flags = poll_flags_from_events(NUM2INT(events));
	
	// fprintf(stderr, "poll_add(%p, %d, %d)\n", sqe, descriptor, flags);
	
	io_uring_prep_poll_add(sqe, descriptor, flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(&data->ring);
	
	struct io_wait_arguments io_wait_arguments = {
		.data = data,
		.fiber = fiber,
		.flags = flags
	};
	
	return rb_rescue(io_wait_transfer, (VALUE)&io_wait_arguments, io_wait_rescue, (VALUE)&io_wait_arguments);
}

inline static
void resize_to_capacity(VALUE string, size_t offset, size_t length) {
	size_t current_length = RSTRING_LEN(string);
	long difference = (long)(offset + length) - (long)current_length;
	
	difference += 1;
	
	if (difference > 0) {
		rb_str_modify_expand(string, difference);
	} else {
		rb_str_modify(string);
	}
}

inline static
void resize_to_fit(VALUE string, size_t offset, size_t length) {
	size_t current_length = RSTRING_LEN(string);
	
	if (current_length < (offset + length)) {
		rb_str_set_len(string, offset + length);
	}
}

VALUE Event_Backend_URing_io_read(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE offset, VALUE length) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	resize_to_capacity(buffer, NUM2SIZET(offset), NUM2SIZET(length));
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	struct iovec iovecs[1];
	iovecs[0].iov_base = RSTRING_PTR(buffer) + NUM2SIZET(offset);
	iovecs[0].iov_len = NUM2SIZET(length);
	
	io_uring_prep_readv(sqe, descriptor, iovecs, 1, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(&data->ring);
	
	// fprintf(stderr, "prep_readv(%p, %d, %ld)\n", sqe, descriptor, iovecs[0].iov_len);
	
	int result = NUM2INT(rb_funcall(data->loop, id_transfer, 0));
	
	if (result < 0) {
		rb_syserr_fail(-result, strerror(-result));
	}
	
	resize_to_fit(buffer, NUM2SIZET(offset), (size_t)result);
	
	return INT2NUM(result);
}

VALUE Event_Backend_URing_io_write(VALUE self, VALUE fiber, VALUE io, VALUE buffer, VALUE offset, VALUE length) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	if ((size_t)RSTRING_LEN(buffer) < NUM2SIZET(offset) + NUM2SIZET(length)) {
		rb_raise(rb_eRuntimeError, "invalid offset/length exceeds bounds of buffer");
	}
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	struct iovec iovecs[1];
	iovecs[0].iov_base = RSTRING_PTR(buffer) + NUM2SIZET(offset);
	iovecs[0].iov_len = NUM2SIZET(length);
	
	io_uring_prep_writev(sqe, descriptor, iovecs, 1, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(&data->ring);
	
	// fprintf(stderr, "prep_writev(%p, %d, %ld)\n", sqe, descriptor, iovecs[0].iov_len);
	
	int result = NUM2INT(rb_funcall(data->loop, id_transfer, 0));
	
	if (result < 0) {
		rb_syserr_fail(-result, strerror(-result));
	}
	
	return INT2NUM(result);
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
		time_t seconds = duration;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

VALUE Event_Backend_URing_select(VALUE self, VALUE duration) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	struct io_uring_cqe *cqes[URING_MAX_EVENTS];
	struct __kernel_timespec storage;
	
	int result = io_uring_peek_batch_cqe(&data->ring, cqes, URING_MAX_EVENTS);
	
	// fprintf(stderr, "result = %d\n", result);
	
	if (result < 0) {
		rb_syserr_fail(-result, strerror(-result));
	} else if (result == 0 && duration != Qnil) {
		result = io_uring_wait_cqes(&data->ring, cqes, 1, make_timeout(duration, &storage), NULL);
		
		// fprintf(stderr, "result (timeout) = %d\n", result);
		
		if (result == -ETIME) {
			result = 0;
		} else if (result < 0) {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	for (int i = 0; i < result; i += 1) {
		VALUE fiber = (VALUE)io_uring_cqe_get_data(cqes[i]);
		VALUE result = INT2NUM(cqes[i]->res);
		
		// fprintf(stderr, "cqes[i]->res = %d\n", cqes[i]->res);
		
		io_uring_cqe_seen(&data->ring, cqes[i]);
		
		rb_funcall(fiber, id_transfer, 1, result);
	}
	
	return INT2NUM(result);
}

VALUE rb_process_status_new(rb_pid_t pid, int status, int error) {
    VALUE last_status = rb_process_status_allocate(rb_cProcessStatus);

    struct rb_process_status *data = RTYPEDDATA_DATA(last_status);
    data->pid = pid;
    data->status = status;
    data->error = error;

    rb_obj_freeze(last_status);
    return last_status;
}

VALUE Event_Backend_URing_process_wait(VALUE self, VALUE fiber, VALUE pid, VALUE flags) {
	pid_t pidv = NUM2PIDT(pid);
	int options = NUM2INT(flags);
	int state = 0;
	int err = 0;

	if ((flags & WNOHANG) > 0) {
		// WNOHANG is nonblock by default.
		pid_t ret = PIDT2NUM(waitpid(pidv, &state, options));
		if (ret == -1) err = errno;
		return rb_process_status_new(pidv, state, err);
	}

	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	struct io_uring_sqe *sqe = io_uring_get_sqe(&data->ring);
	
	int descriptor = pidfd_open(pidv, 0);
	short poll_flags = POLLIN | POLLRDNORM;
	io_uring_prep_poll_add(sqe, descriptor, poll_flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(&data->ring);
	
	rb_funcall(data->loop, id_transfer, 0);
	pid_t ret = PIDT2NUM(waitpid(pidv, &state, options));
	if (ret == -1) err = errno;
	return rb_process_status_new(pidv, state, err);
}

void Init_Event_Backend_URing(VALUE Event_Backend) {
	id_fileno = rb_intern("fileno");
	id_transfer = rb_intern("transfer");
	
	Event_Backend_URing = rb_define_class_under(Event_Backend, "URing", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_URing, Event_Backend_URing_allocate);
	rb_define_method(Event_Backend_URing, "initialize", Event_Backend_URing_initialize, 1);
	
	rb_define_method(Event_Backend_URing, "io_wait", Event_Backend_URing_io_wait, 3);
	rb_define_method(Event_Backend_URing, "select", Event_Backend_URing_select, 1);
	
	rb_define_method(Event_Backend_URing, "io_read", Event_Backend_URing_io_read, 5);
	rb_define_method(Event_Backend_URing, "io_write", Event_Backend_URing_io_write, 5);
	rb_define_method(Event_Backend_URing, "process_wait", Event_Backend_URing_process_wait, 3);
}
