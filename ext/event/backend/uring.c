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

#include <liburing.h>
#include <time.h>

static VALUE Event_Backend_URing = Qnil;
static ID id_fileno, id_transfer;

static const int READABLE = 1, PRIORITY = 2, WRITABLE = 4;

static const int URING_ENTRIES = 1024;
static const int URING_MAX_EVENTS = 1024;

struct Event_Backend_URing {
	VALUE loop;
	struct io_uring* ring;
};

void Event_Backend_URing_Type_mark(void *_data)
{
	struct Event_Backend_URing *data = _data;
	rb_gc_mark(data->loop);
}

void Event_Backend_URing_Type_free(void *_data)
{
	struct Event_Backend_URing *data = _data;
	
	if (data->ring) {
		io_uring_queue_exit(data->ring);
		xfree(data->ring);
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
	data->ring = NULL;
	
	return instance;
}

VALUE Event_Backend_URing_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	data->loop = loop;
	data->ring = xmalloc(sizeof(struct io_uring));
	
	int result = io_uring_queue_init(URING_ENTRIES, data->ring, 0);
	
	if (result == -1) {
		rb_sys_fail("io_uring_queue_init");
	}
	
	return self;
}

VALUE Event_Backend_URing_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	struct io_uring_sqe *sqe = io_uring_get_sqe(data->ring);
	
	int mask = NUM2INT(events);
	short flags = 0;
	
	if (mask & READABLE) {
		flags |= POLL_IN;
	}
	
	if (mask & PRIORITY) {
		flags |= POLL_PRI;
	}
	
	if (mask & WRITABLE) {
		flags |= POLL_OUT;
	}

	// fprintf(stderr, "poll_add(%p, %d, %d)\n", sqe, descriptor, flags);

	io_uring_prep_poll_add(sqe, descriptor, flags);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(data->ring);
	
	// fprintf(stderr, "count = %d, errno = %d\n", count, errno);

	rb_funcall(data->loop, id_transfer, 0);
	
	return Qnil;
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
	struct io_uring_sqe *sqe = io_uring_get_sqe(data->ring);

	struct iovec iovecs[1];
	iovecs[0].iov_base = RSTRING_PTR(buffer) + NUM2SIZET(offset);
	iovecs[0].iov_len = NUM2SIZET(length);

	io_uring_prep_readv(sqe, descriptor, iovecs, 1, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(data->ring);
	
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
	struct io_uring_sqe *sqe = io_uring_get_sqe(data->ring);

	struct iovec iovecs[1];
	iovecs[0].iov_base = RSTRING_PTR(buffer) + NUM2SIZET(offset);
	iovecs[0].iov_len = NUM2SIZET(length);

	io_uring_prep_writev(sqe, descriptor, iovecs, 1, 0);
	io_uring_sqe_set_data(sqe, (void*)fiber);
	io_uring_submit(data->ring);
	
	int result = NUM2INT(rb_funcall(data->loop, id_transfer, 0));

	if (result < 0) {
		rb_syserr_fail(-result, strerror(-result));
	}

	return INT2NUM(result);
}

VALUE Event_Backend_URing_select(VALUE self, VALUE duration) {
	struct Event_Backend_URing *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_URing, &Event_Backend_URing_Type, data);
	
	struct io_uring_cqe *cqes[URING_MAX_EVENTS];
	struct __kernel_timespec storage;

	if (duration != Qnil) {
		int result = io_uring_wait_cqe_timeout(data->ring, cqes, make_timeout(duration, &storage));
		
		if (result == -ETIME) {
			// Timeout.
		} else if (result < 0) {
			rb_syserr_fail(-result, strerror(-result));
		}
	}
	
	int count = io_uring_peek_batch_cqe(data->ring, cqes, URING_MAX_EVENTS);
	
	if (count == -1) {
		rb_sys_fail("io_uring_peek_batch_cqe");
	}
	
	for (int i = 0; i < count; i += 1) {
		VALUE fiber = (VALUE)io_uring_cqe_get_data(cqes[i]);
		VALUE result = INT2NUM(cqes[i]->res);

		io_uring_cqe_seen(data->ring, cqes[i]);
		
		rb_funcall(fiber, id_transfer, 1, result);
	}
	
	return INT2NUM(count);
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
}
