// Released under the MIT License.
// Copyright, 2026, by Samuel Williams.

#include "futex.h"

#ifdef IO_EVENT_FUTEX

#include <errno.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <ruby/fiber/scheduler.h>
#include <ruby/io/buffer.h>
#include <ruby/thread.h>

struct IO_Event_Futex {
	VALUE buffer;
	uint32_t *address;
};

static const rb_data_type_t IO_Event_Futex_Type;

static void IO_Event_Futex_mark(void *_futex) {
	struct IO_Event_Futex *futex = _futex;
	rb_gc_mark_movable(futex->buffer);
}

static void IO_Event_Futex_compact(void *_futex) {
	struct IO_Event_Futex *futex = _futex;
	futex->buffer = rb_gc_location(futex->buffer);
}

static void IO_Event_Futex_free(void *_futex) {
	xfree(_futex);
}

static size_t IO_Event_Futex_size(const void *_futex) {
	return sizeof(struct IO_Event_Futex);
}

static const rb_data_type_t IO_Event_Futex_Type = {
	.wrap_struct_name = "IO::Event::Futex",
	.function = {
		.dmark = IO_Event_Futex_mark,
		.dcompact = IO_Event_Futex_compact,
		.dfree = IO_Event_Futex_free,
		.dsize = IO_Event_Futex_size,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

static VALUE IO_Event_Futex_allocate(VALUE klass) {
	struct IO_Event_Futex *futex = NULL;
	VALUE instance = TypedData_Make_Struct(klass, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	futex->buffer = Qnil;
	futex->address = NULL;
	return instance;
}

static ID id_offset;
static ID id_futex_wait;
static ID id_futex_waitv;

static VALUE IO_Event_Futex_initialize(int argc, VALUE *argv, VALUE self) {
	VALUE buffer, options;
	rb_scan_args(argc, argv, "1:", &buffer, &options);

	VALUE offset_value = Qundef;
	if (!NIL_P(options)) {
		ID keys[] = {id_offset};
		VALUE values[1];
		rb_get_kwargs(options, keys, 0, 1, values);
		offset_value = values[0];
	}

	size_t offset = offset_value == Qundef ? 0 : NUM2SIZET(offset_value);
	void *base = NULL;
	size_t size = 0;
	rb_io_buffer_get_bytes_for_writing(buffer, &base, &size);

	if (offset > size || size - offset < sizeof(uint32_t)) {
		rb_raise(rb_eRangeError, "Futex offset exceeds the buffer size!");
	}

	uint32_t *address = (uint32_t *)((char *)base + offset);
	if ((uintptr_t)address % sizeof(uint32_t) != 0) {
		rb_raise(rb_eArgError, "Futex address must be aligned to 4 bytes!");
	}

	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	RB_OBJ_WRITE(self, &futex->buffer, buffer);
	futex->address = address;

	return self;
}

static VALUE IO_Event_Futex_value(VALUE self) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	return UINT2NUM(__atomic_load_n(futex->address, __ATOMIC_ACQUIRE));
}

static VALUE IO_Event_Futex_set_value(VALUE self, VALUE value) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	uint32_t converted = NUM2UINT(value);
	__atomic_store_n(futex->address, converted, __ATOMIC_RELEASE);
	return value;
}

static VALUE IO_Event_Futex_increment(int argc, VALUE *argv, VALUE self) {
	VALUE amount_value;
	rb_scan_args(argc, argv, "01", &amount_value);
	uint32_t amount = NIL_P(amount_value) ? 1 : NUM2UINT(amount_value);

	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	uint32_t value = __atomic_add_fetch(futex->address, amount, __ATOMIC_ACQ_REL);
	return UINT2NUM(value);
}

static VALUE IO_Event_Futex_wake(int argc, VALUE *argv, VALUE self) {
	VALUE count_value;
	rb_scan_args(argc, argv, "01", &count_value);
	int count = NIL_P(count_value) ? 1 : NUM2INT(count_value);
	if (count < 0) rb_raise(rb_eArgError, "Wake count must be non-negative!");

	int result = syscall(SYS_futex, IO_Event_Futex_address(self), FUTEX_WAKE, count, NULL, NULL, 0);
	if (result < 0) rb_sys_fail("IO_Event_Futex_wake:futex");
	return INT2NUM(result);
}

static VALUE IO_Event_Futex_signal(int argc, VALUE *argv, VALUE self) {
	VALUE count_value;
	rb_scan_args(argc, argv, "01", &count_value);

	VALUE value = IO_Event_Futex_increment(0, NULL, self);
	VALUE arguments[] = {NIL_P(count_value) ? INT2NUM(1) : count_value};
	IO_Event_Futex_wake(1, arguments, self);
	return value;
}

uint32_t *IO_Event_Futex_address(VALUE self) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	return futex->address;
}

struct IO_Event_Futex_BlockingWait {
	uint32_t *address;
	uint32_t expected;
	int result;
	int error;
};

static void *IO_Event_Futex_blocking_wait_without_gvl(void *_arguments) {
	struct IO_Event_Futex_BlockingWait *arguments = _arguments;
	arguments->result = syscall(SYS_futex, arguments->address, FUTEX_WAIT, arguments->expected, NULL, NULL, 0);
	arguments->error = arguments->result < 0 ? errno : 0;
	return NULL;
}

static VALUE IO_Event_Futex_blocking_wait(VALUE self, VALUE expected_value) {
	struct IO_Event_Futex_BlockingWait arguments = {
		.address = IO_Event_Futex_address(self),
		.expected = NUM2UINT(expected_value),
	};

	rb_thread_call_without_gvl(IO_Event_Futex_blocking_wait_without_gvl, &arguments, RUBY_UBF_IO, 0);

	if (arguments.result == 0) {
		return Qtrue;
	} else if (arguments.error == EAGAIN) {
		return Qfalse;
	} else {
		rb_syserr_fail(arguments.error, "IO_Event_Futex_blocking_wait:futex");
	}

	return Qfalse;
}

#ifdef SYS_futex_waitv

struct IO_Event_Futex_BlockingWaitV {
	struct futex_waitv *vector;
	size_t count;
	int result;
	int error;
};

static void *IO_Event_Futex_blocking_waitv_without_gvl(void *_arguments) {
	struct IO_Event_Futex_BlockingWaitV *arguments = _arguments;
	arguments->result = syscall(SYS_futex_waitv, arguments->vector, arguments->count, 0, NULL, CLOCK_MONOTONIC);
	arguments->error = arguments->result < 0 ? errno : 0;
	return NULL;
}

static VALUE IO_Event_Futex_blocking_waitv(VALUE entries) {
	entries = rb_Array(entries);
	long count = RARRAY_LEN(entries);
	if (count < 1 || count > FUTEX_WAITV_MAX) {
		rb_raise(rb_eArgError, "Futex vector must contain between 1 and %d entries!", FUTEX_WAITV_MAX);
	}

	struct futex_waitv *vector = ALLOCA_N(struct futex_waitv, count);
	for (long index = 0; index < count; index++) {
		VALUE entry = rb_Array(RARRAY_AREF(entries, index));
		if (RARRAY_LEN(entry) != 2) {
			rb_raise(rb_eArgError, "Each futex vector entry must contain a futex and its expected value!");
		}

		VALUE futex = RARRAY_AREF(entry, 0);
		vector[index].val = NUM2UINT(RARRAY_AREF(entry, 1));
		vector[index].uaddr = (uintptr_t)IO_Event_Futex_address(futex);
		vector[index].flags = FUTEX_32;
		vector[index].__reserved = 0;
	}

	struct IO_Event_Futex_BlockingWaitV arguments = {
		.vector = vector,
		.count = count,
	};

	rb_thread_call_without_gvl(IO_Event_Futex_blocking_waitv_without_gvl, &arguments, RUBY_UBF_IO, 0);
	RB_GC_GUARD(entries);

	if (arguments.result >= 0) {
		return INT2NUM(arguments.result);
	} else if (arguments.error == EAGAIN) {
		return Qnil;
	} else {
		rb_syserr_fail(arguments.error, "IO_Event_Futex_blocking_waitv:futex_waitv");
	}

	return Qnil;
}

#endif

static VALUE IO_Event_Futex_wait(int argc, VALUE *argv, VALUE self) {
	VALUE expected_value;
	rb_scan_args(argc, argv, "01", &expected_value);

	if (argc == 0) {
		expected_value = IO_Event_Futex_value(self);
	}

	VALUE scheduler = rb_fiber_scheduler_current();
	if (NIL_P(scheduler)) {
		return IO_Event_Futex_blocking_wait(self, expected_value);
	}

	if (!rb_respond_to(scheduler, id_futex_wait)) {
		rb_raise(rb_eNotImpError, "The current fiber scheduler does not support futex waits!");
	}

	return rb_funcall(scheduler, id_futex_wait, 2, self, expected_value);
}

#ifdef SYS_futex_waitv

static VALUE IO_Event_Futex_wait_any(VALUE klass, VALUE entries) {
	(void)klass;
	VALUE scheduler = rb_fiber_scheduler_current();
	if (NIL_P(scheduler)) {
		return IO_Event_Futex_blocking_waitv(entries);
	}

	if (!rb_respond_to(scheduler, id_futex_waitv)) {
		rb_raise(rb_eNotImpError, "The current fiber scheduler does not support vector futex waits!");
	}

	return rb_funcall(scheduler, id_futex_waitv, 1, entries);
}

#endif

void Init_IO_Event_Futex(VALUE IO_Event) {
	VALUE IO_Event_Futex = rb_define_class_under(IO_Event, "Futex", rb_cObject);
	rb_define_alloc_func(IO_Event_Futex, IO_Event_Futex_allocate);
	rb_define_method(IO_Event_Futex, "initialize", IO_Event_Futex_initialize, -1);
	rb_define_method(IO_Event_Futex, "value", IO_Event_Futex_value, 0);
	rb_define_method(IO_Event_Futex, "value=", IO_Event_Futex_set_value, 1);
	rb_define_method(IO_Event_Futex, "increment", IO_Event_Futex_increment, -1);
	rb_define_method(IO_Event_Futex, "wake", IO_Event_Futex_wake, -1);
	rb_define_method(IO_Event_Futex, "signal", IO_Event_Futex_signal, -1);
	rb_define_method(IO_Event_Futex, "wait", IO_Event_Futex_wait, -1);

#ifdef SYS_futex_waitv
	rb_define_const(IO_Event_Futex, "WAITV_LIMIT", INT2NUM(FUTEX_WAITV_MAX));
	rb_define_singleton_method(IO_Event_Futex, "wait_any", IO_Event_Futex_wait_any, 1);
#endif

	id_offset = rb_intern("offset");
	id_futex_wait = rb_intern("futex_wait");
	id_futex_waitv = rb_intern("futex_waitv");
}

#endif
