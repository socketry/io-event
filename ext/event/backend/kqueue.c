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

#include <sys/event.h>
#include <time.h>

static VALUE Event_Backend_KQueue = Qnil;
static ID id_fileno, id_transfer;

const int READABLE = 1, PRIORITY = 2, WRITABLE = 4;

const unsigned KQUEUE_MAX_EVENTS = 1024;

struct Event_Backend_KQueue {
	VALUE loop;
	int descriptor;
};

void Event_Backend_KQueue_Type_mark(void *_data)
{
	struct Event_Backend_KQueue *data = _data;
	rb_gc_mark(data->loop);
}

void Event_Backend_KQueue_Type_free(void *_data)
{
	struct Event_Backend_KQueue *data = _data;
	
	if (data->descriptor >= 0) {
		close(data->descriptor);
	}
	
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
	
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Backend_KQueue_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	data->loop = loop;
	data->descriptor = kqueue();
	
	return self;
}

VALUE Event_Backend_KQueue_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	struct kevent event;
	u_short flags = 0;

	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	
	int mask = NUM2INT(events);
	
	if (mask & READABLE) {
		flags |= EVFILT_READ;
	}
	
	if (mask & PRIORITY) {
		flags |= EV_OOBAND;
	}
	
	if (mask & WRITABLE) {
		flags |= EVFILT_WRITE;
	}

	EV_SET(&event, descriptor, flags, EV_ADD|EV_ENABLE|EV_ONESHOT, 0, 0, (void*)fiber);
	
	// A better approach is to batch all changes:
	int result = kevent(data->descriptor, &event, 1, NULL, 0, NULL);
	
	if (result == -1) {
		rb_sys_fail("kevent");
	}
	
	rb_funcall(data->loop, id_transfer, 0);
	
	return Qnil;
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
		time_t seconds = duration;
		
		storage->tv_sec = seconds;
		storage->tv_nsec = (value - seconds) * 1000000000L;
		
		return storage;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

VALUE Event_Backend_KQueue_select(VALUE self, VALUE duration) {
	struct Event_Backend_KQueue *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_KQueue, &Event_Backend_KQueue_Type, data);
	
	struct kevent events[KQUEUE_MAX_EVENTS];
	struct timespec storage;
	
	int count = kevent(data->descriptor, NULL, 0, events, KQUEUE_MAX_EVENTS, make_timeout(duration, &storage));
	
	if (count == -1) {
		rb_sys_fail("kevent");
	}
	
	for (int i = 0; i < count; i += 1) {
		VALUE fiber = (VALUE)events[i].udata;
		rb_funcall(fiber, id_transfer, 0);
	}
	
	return INT2NUM(count);
}

void Init_Event_Backend_KQueue(VALUE Event_Backend) {
	id_fileno = rb_intern("fileno");
	id_transfer = rb_intern("transfer");
	
	Event_Backend_KQueue = rb_define_class_under(Event_Backend, "KQueue", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_KQueue, Event_Backend_KQueue_allocate);
	rb_define_method(Event_Backend_KQueue, "initialize", Event_Backend_KQueue_initialize, 1);
	
	rb_define_method(Event_Backend_KQueue, "io_wait", Event_Backend_KQueue_io_wait, 3);
	rb_define_method(Event_Backend_KQueue, "select", Event_Backend_KQueue_select, 1);
}
