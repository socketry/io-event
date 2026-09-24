// Released under the MIT License.
// Copyright, 2026, by Samuel Williams.

#include "futex.h"

#ifdef IO_EVENT_FUTEX

#include <errno.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <ruby/fiber/scheduler.h>
#include <ruby/io/buffer.h>
#include <ruby/thread.h>

// The finalizer owns the allocation lock independently of the Futex. It must
// never retain the Futex or its C struct: either may be reclaimed before the
// finalizer runs. Keeping the buffer here preserves it until it is unlocked.
struct IO_Event_Futex_Finalizer {
	VALUE buffer;
	uint32_t *address;
	size_t waits;
};

static VALUE IO_Event_Futex_Finalizer_Class;

static void IO_Event_Futex_Finalizer_mark(void *_finalizer) {
	struct IO_Event_Futex_Finalizer *finalizer = _finalizer;
	rb_gc_mark_movable(finalizer->buffer);
}

static void IO_Event_Futex_Finalizer_compact(void *_finalizer) {
	struct IO_Event_Futex_Finalizer *finalizer = _finalizer;
	finalizer->buffer = rb_gc_location(finalizer->buffer);
}

static size_t IO_Event_Futex_Finalizer_size(const void *_finalizer) {
	return sizeof(struct IO_Event_Futex_Finalizer);
}

static const rb_data_type_t IO_Event_Futex_Finalizer_Type = {
	.wrap_struct_name = "IO::Event::Futex::Finalizer",
	.function = {
		.dmark = IO_Event_Futex_Finalizer_mark,
		.dcompact = IO_Event_Futex_Finalizer_compact,
		.dfree = RUBY_TYPED_DEFAULT_FREE,
		.dsize = IO_Event_Futex_Finalizer_size,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

static VALUE IO_Event_Futex_Finalizer_release(VALUE self) {
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	if (!NIL_P(finalizer->buffer)) {
		rb_io_buffer_unlock(finalizer->buffer);
		RB_OBJ_WRITE(self, &finalizer->buffer, Qnil);
		finalizer->address = NULL;
	}
	return Qnil;
}

static VALUE IO_Event_Futex_Finalizer_call(VALUE self, VALUE object_id) {
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	// Ruby also invokes finalizers at shutdown, even for reachable objects.
	// Never unlock memory still used by an outstanding kernel operation.
	if (finalizer->waits) return Qnil;
	return IO_Event_Futex_Finalizer_release(self);
}

struct IO_Event_Futex {
	VALUE finalizer;
};

static void IO_Event_Futex_mark(void *_futex) {
	struct IO_Event_Futex *futex = _futex;
	rb_gc_mark_movable(futex->finalizer);
}

static void IO_Event_Futex_compact(void *_futex) {
	struct IO_Event_Futex *futex = _futex;
	futex->finalizer = rb_gc_location(futex->finalizer);
}

static size_t IO_Event_Futex_size(const void *_futex) {
	return sizeof(struct IO_Event_Futex);
}

static const rb_data_type_t IO_Event_Futex_Type = {
	.wrap_struct_name = "IO::Event::Futex",
	.function = {
		.dmark = IO_Event_Futex_mark,
		.dcompact = IO_Event_Futex_compact,
		.dfree = RUBY_TYPED_DEFAULT_FREE,
		.dsize = IO_Event_Futex_size,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

static VALUE IO_Event_Futex_allocate(VALUE klass) {
	struct IO_Event_Futex *futex = NULL;
	VALUE instance = TypedData_Make_Struct(klass, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	futex->finalizer = Qnil;
	return instance;
}

static ID id_offset;
static ID id_futex_wait;
static ID id_futex_waitv;

static VALUE IO_Event_Futex_initialize(int argc, VALUE *argv, VALUE self) {
	rb_check_frozen(self);
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	if (!NIL_P(futex->finalizer)) rb_raise(rb_eRuntimeError, "Futex is already initialized!");

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
	// Numeric coercion may have invoked initialize recursively.
	if (!NIL_P(futex->finalizer)) rb_raise(rb_eRuntimeError, "Futex is already initialized!");

	// Register cleanup before acquiring the lock. All allocating operations are
	// performed before extracting the pointer; failures leave an inert finalizer.
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	VALUE finalizer_value = TypedData_Make_Struct(IO_Event_Futex_Finalizer_Class, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	finalizer->buffer = Qnil;
	finalizer->address = NULL;
	finalizer->waits = 0;
	rb_define_finalizer(self, finalizer_value);

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

	// Nothing after a successful lock can raise or invoke Ruby code.
	rb_io_buffer_lock(buffer);
	RB_OBJ_WRITE(finalizer_value, &finalizer->buffer, buffer);
	finalizer->address = address;
	RB_OBJ_WRITE(self, &futex->finalizer, finalizer_value);

	return self;
}

static VALUE IO_Event_Futex_value(VALUE self) {
	return UINT2NUM(__atomic_load_n(IO_Event_Futex_address(self), __ATOMIC_ACQUIRE));
}

static VALUE IO_Event_Futex_set_value(VALUE self, VALUE value) {
	uint32_t converted = NUM2UINT(value);
	__atomic_store_n(IO_Event_Futex_address(self), converted, __ATOMIC_RELEASE);
	return value;
}

static VALUE IO_Event_Futex_increment(int argc, VALUE *argv, VALUE self) {
	VALUE amount_value;
	rb_scan_args(argc, argv, "01", &amount_value);
	uint32_t amount = NIL_P(amount_value) ? 1 : NUM2UINT(amount_value);

	uint32_t value = __atomic_add_fetch(IO_Event_Futex_address(self), amount, __ATOMIC_ACQ_REL);
	return UINT2NUM(value);
}

static VALUE IO_Event_Futex_decrement(int argc, VALUE *argv, VALUE self) {
	VALUE amount_value;
	rb_scan_args(argc, argv, "01", &amount_value);
	uint32_t amount = NIL_P(amount_value) ? 1 : NUM2UINT(amount_value);

	uint32_t value = __atomic_sub_fetch(IO_Event_Futex_address(self), amount, __ATOMIC_ACQ_REL);
	return UINT2NUM(value);
}

static VALUE IO_Event_Futex_compare_exchange(VALUE self, VALUE expected_value, VALUE desired_value) {
	uint32_t expected = NUM2UINT(expected_value);
	uint32_t desired = NUM2UINT(desired_value);

	bool exchanged = __atomic_compare_exchange_n(
		IO_Event_Futex_address(self),
		&expected,
		desired,
		false,
		__ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE
	);

	return exchanged ? Qtrue : Qfalse;
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

	int count = NIL_P(count_value) ? 1 : NUM2INT(count_value);
	if (count < 0) rb_raise(rb_eArgError, "Wake count must be non-negative!");
	uint32_t *address = IO_Event_Futex_address(self);
	uint32_t value = __atomic_add_fetch(address, 1, __ATOMIC_ACQ_REL);
	int result = syscall(SYS_futex, address, FUTEX_WAKE, count, NULL, NULL, 0);
	if (result < 0) rb_sys_fail("IO_Event_Futex_signal:futex");
	return UINT2NUM(value);
}

static struct IO_Event_Futex_Finalizer *IO_Event_Futex_get(VALUE self) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	if (NIL_P(futex->finalizer)) rb_raise(rb_eIOError, "Futex is uninitialized!");
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	TypedData_Get_Struct(futex->finalizer, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	if (!finalizer->address) rb_raise(rb_eIOError, "Futex is closed!");
	return finalizer;
}

uint32_t *IO_Event_Futex_address(VALUE self) {
	return IO_Event_Futex_get(self)->address;
}

uint32_t *IO_Event_Futex_acquire(VALUE self) {
	struct IO_Event_Futex_Finalizer *finalizer = IO_Event_Futex_get(self);
	if (finalizer->waits == SIZE_MAX) rb_raise(rb_eRuntimeError, "Too many futex waits!");
	finalizer->waits += 1;
	return finalizer->address;
}

void IO_Event_Futex_release(VALUE self) {
	struct IO_Event_Futex_Finalizer *finalizer = IO_Event_Futex_get(self);
	if (!finalizer->waits) rb_bug("IO_Event_Futex_release: no pending waits");
	finalizer->waits -= 1;
}

static VALUE IO_Event_Futex_close(VALUE self) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	if (NIL_P(futex->finalizer)) return Qnil;
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	TypedData_Get_Struct(futex->finalizer, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	if (finalizer->waits) rb_raise(rb_eIOError, "Cannot close a futex with pending waits!");
	return IO_Event_Futex_Finalizer_release(futex->finalizer);
}

static VALUE IO_Event_Futex_closed_p(VALUE self) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	if (NIL_P(futex->finalizer)) return Qtrue;
	struct IO_Event_Futex_Finalizer *finalizer = NULL;
	TypedData_Get_Struct(futex->finalizer, struct IO_Event_Futex_Finalizer, &IO_Event_Futex_Finalizer_Type, finalizer);
	return finalizer->address ? Qfalse : Qtrue;
}

static VALUE IO_Event_Futex_initialize_copy(VALUE self, VALUE other) {
	struct IO_Event_Futex *futex = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Futex, &IO_Event_Futex_Type, futex);
	// Ruby copies finalizers before invoking initialize_copy. The rejected copy
	// must not release the original's allocation lock when it is collected.
	if (NIL_P(futex->finalizer)) rb_undefine_finalizer(self);
	rb_raise(rb_eTypeError, "Cannot copy a futex!");
}

struct IO_Event_Futex_BlockingWait {
	VALUE futex;
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

static VALUE IO_Event_Futex_blocking_wait_body(VALUE _arguments) {
	struct IO_Event_Futex_BlockingWait *arguments = (void *)_arguments;
	arguments->address = IO_Event_Futex_acquire(arguments->futex);
	rb_thread_call_without_gvl(IO_Event_Futex_blocking_wait_without_gvl, arguments, RUBY_UBF_IO, 0);

	if (arguments->result == 0) {
		return Qtrue;
	} else if (arguments->error == EAGAIN) {
		return Qfalse;
	} else {
		rb_syserr_fail(arguments->error, "IO_Event_Futex_blocking_wait:futex");
	}

	return Qfalse;
}

static VALUE IO_Event_Futex_blocking_wait_ensure(VALUE _arguments) {
	struct IO_Event_Futex_BlockingWait *arguments = (void *)_arguments;
	if (arguments->address) IO_Event_Futex_release(arguments->futex);
	return Qnil;
}

static VALUE IO_Event_Futex_blocking_wait(VALUE self, VALUE expected_value) {
	struct IO_Event_Futex_BlockingWait arguments = {
		.futex = self,
		.expected = NUM2UINT(expected_value),
	};
	VALUE result = rb_ensure(IO_Event_Futex_blocking_wait_body, (VALUE)&arguments, IO_Event_Futex_blocking_wait_ensure, (VALUE)&arguments);
	RB_GC_GUARD(self);
	return result;
}

#ifdef IO_EVENT_FUTEX_WAITV

// Coerce all arguments before retaining native addresses. The returned array
// owns a private snapshot of the futex references: mutating the caller's entries
// while a wait is pending must not make its futexes collectible.
VALUE IO_Event_Futex_prepare_waitv(VALUE entries, struct futex_waitv *vector) {
	entries = rb_ary_dup(rb_Array(entries));
	long count = RARRAY_LEN(entries);
	if (count < 1 || count > FUTEX_WAITV_MAX) {
		rb_raise(rb_eArgError, "Futex vector must contain between 1 and %d entries!", FUTEX_WAITV_MAX);
	}
	VALUE futexes = rb_ary_new_capa(count);
	for (long index = 0; index < count; index++) {
		VALUE entry = rb_Array(RARRAY_AREF(entries, index));
		if (RARRAY_LEN(entry) != 2) {
			rb_raise(rb_eArgError, "Each futex vector entry must contain a futex and its expected value!");
		}
		rb_ary_push(futexes, RARRAY_AREF(entry, 0));
		vector[index].val = NUM2UINT(RARRAY_AREF(entry, 1));
		vector[index].flags = FUTEX_32;
		vector[index].__reserved = 0;
	}
	return futexes;
}

struct IO_Event_Futex_BlockingWaitV {
	VALUE futexes;
	struct futex_waitv *vector;
	size_t count;
	size_t acquired;
	int result;
	int error;
};

static void *IO_Event_Futex_blocking_waitv_without_gvl(void *_arguments) {
	struct IO_Event_Futex_BlockingWaitV *arguments = _arguments;
	arguments->result = syscall(SYS_futex_waitv, arguments->vector, arguments->count, 0, NULL, CLOCK_MONOTONIC);
	arguments->error = arguments->result < 0 ? errno : 0;
	return NULL;
}

static VALUE IO_Event_Futex_blocking_waitv_body(VALUE _arguments) {
	struct IO_Event_Futex_BlockingWaitV *arguments = (void *)_arguments;
	while (arguments->acquired < arguments->count) {
		size_t index = arguments->acquired;
		arguments->vector[index].uaddr = (uintptr_t)IO_Event_Futex_acquire(RARRAY_AREF(arguments->futexes, index));
		arguments->acquired += 1;
	}
	rb_thread_call_without_gvl(IO_Event_Futex_blocking_waitv_without_gvl, arguments, RUBY_UBF_IO, 0);

	if (arguments->result >= 0) {
		return INT2NUM(arguments->result);
	} else if (arguments->error == EAGAIN) {
		return Qnil;
	} else {
		rb_syserr_fail(arguments->error, "IO_Event_Futex_blocking_waitv:futex_waitv");
	}

	return Qnil;
}

static VALUE IO_Event_Futex_blocking_waitv_ensure(VALUE _arguments) {
	struct IO_Event_Futex_BlockingWaitV *arguments = (void *)_arguments;
	while (arguments->acquired) {
		IO_Event_Futex_release(RARRAY_AREF(arguments->futexes, --arguments->acquired));
	}
	return Qnil;
}

static VALUE IO_Event_Futex_blocking_waitv(VALUE entries) {
	struct futex_waitv vector[FUTEX_WAITV_MAX];
	VALUE futexes = IO_Event_Futex_prepare_waitv(entries, vector);
	struct IO_Event_Futex_BlockingWaitV arguments = {
		.futexes = futexes,
		.vector = vector,
		.count = RARRAY_LEN(futexes),
	};
	VALUE result = rb_ensure(IO_Event_Futex_blocking_waitv_body, (VALUE)&arguments, IO_Event_Futex_blocking_waitv_ensure, (VALUE)&arguments);
	RB_GC_GUARD(futexes);
	return result;
}

#endif

static VALUE IO_Event_Futex_wait(VALUE self, VALUE expected_value) {
	VALUE scheduler = rb_fiber_scheduler_current();
	if (NIL_P(scheduler)) {
		return IO_Event_Futex_blocking_wait(self, expected_value);
	}

	if (!rb_respond_to(scheduler, id_futex_wait)) {
		rb_raise(rb_eNotImpError, "The current fiber scheduler does not support futex waits!");
	}

	return rb_funcall(scheduler, id_futex_wait, 2, self, expected_value);
}

#ifdef IO_EVENT_FUTEX_WAITV

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
	IO_Event_Futex_Finalizer_Class = rb_class_new(rb_cObject);
	rb_gc_register_mark_object(IO_Event_Futex_Finalizer_Class);
	rb_undef_alloc_func(IO_Event_Futex_Finalizer_Class);
	rb_define_method(IO_Event_Futex_Finalizer_Class, "call", IO_Event_Futex_Finalizer_call, 1);

	VALUE IO_Event_Futex = rb_define_class_under(IO_Event, "Futex", rb_cObject);
	rb_define_alloc_func(IO_Event_Futex, IO_Event_Futex_allocate);
	rb_define_method(IO_Event_Futex, "initialize", IO_Event_Futex_initialize, -1);
	rb_define_method(IO_Event_Futex, "initialize_copy", IO_Event_Futex_initialize_copy, 1);
	rb_define_method(IO_Event_Futex, "close", IO_Event_Futex_close, 0);
	rb_define_method(IO_Event_Futex, "closed?", IO_Event_Futex_closed_p, 0);
	rb_define_method(IO_Event_Futex, "value", IO_Event_Futex_value, 0);
	rb_define_method(IO_Event_Futex, "value=", IO_Event_Futex_set_value, 1);
	rb_define_method(IO_Event_Futex, "increment", IO_Event_Futex_increment, -1);
	rb_define_method(IO_Event_Futex, "decrement", IO_Event_Futex_decrement, -1);
	rb_define_method(IO_Event_Futex, "compare_exchange", IO_Event_Futex_compare_exchange, 2);
	rb_define_method(IO_Event_Futex, "wake", IO_Event_Futex_wake, -1);
	rb_define_method(IO_Event_Futex, "signal", IO_Event_Futex_signal, -1);
	rb_define_method(IO_Event_Futex, "wait", IO_Event_Futex_wait, 1);

#ifdef IO_EVENT_FUTEX_WAITV
	rb_define_const(IO_Event_Futex, "WAITV_LIMIT", INT2NUM(FUTEX_WAITV_MAX));
	rb_define_singleton_method(IO_Event_Futex, "wait_any", IO_Event_Futex_wait_any, 1);
#endif

	id_offset = rb_intern("offset");
	id_futex_wait = rb_intern("futex_wait");
	id_futex_waitv = rb_intern("futex_waitv");
}

#endif
