// Released under the MIT License.
// Copyright, 2021-2025, by Samuel Williams.

#include "selector.h"

#include <fcntl.h>
#include <stdlib.h>

static const int DEBUG = 0;

#ifndef RB_NOGVL_PENDING_INTR_FAIL
static ID handle_interrupt_id;
static VALUE signal_exception_never = Qnil;

struct IO_Event_Selector_blocking_operation_arguments {
	void *(*function)(void *);
	void *data;
	rb_unblock_function_t *unblock_function;
	void *unblock_data;
};

static VALUE IO_Event_Selector_blocking_operation_yield(RB_BLOCK_CALL_FUNC_ARGLIST(yielded_argument, callback_argument)) {
	struct IO_Event_Selector_blocking_operation_arguments *arguments = (struct IO_Event_Selector_blocking_operation_arguments *)callback_argument;
	
	rb_thread_call_without_gvl2(arguments->function, arguments->data, arguments->unblock_function, arguments->unblock_data);
	
	return Qnil;
}

static VALUE IO_Event_Selector_blocking_operation_fallback(VALUE callback_argument) {
	return rb_block_call(rb_cThread, handle_interrupt_id, 1, &signal_exception_never, IO_Event_Selector_blocking_operation_yield, callback_argument);
}
#endif

void IO_Event_Selector_blocking_operation(struct IO_Event_Selector *selector, void *(*function)(void *), void *data, rb_unblock_function_t *unblock_function, void *unblock_data) {
	int state = 0;
	selector->blocked = 1;
	
#ifdef RB_NOGVL_PENDING_INTR_FAIL
	rb_nogvl(function, data, unblock_function, unblock_data, RB_NOGVL_INTR_FAIL | RB_NOGVL_PENDING_INTR_FAIL);
#else
	// `Thread.handle_interrupt` resets `pending_interrupt_queue_checked` and
	// raises the VM interrupt flag when its mask is pushed with a non-empty
	// pending queue. Its C block calls `rb_thread_call_without_gvl2` directly,
	// without a Ruby safepoint between refreshing the flag and entering
	// `unblock_function_set`. That function either observes the interrupt or
	// installs the UBF while holding Ruby's interrupt lock.
	//
	// Keep signal exceptions deferred here: this fallback should prevent a
	// stranded native wait without changing the caller's delivery timing.
	struct IO_Event_Selector_blocking_operation_arguments arguments = {
		.function = function,
		.data = data,
		.unblock_function = unblock_function,
		.unblock_data = unblock_data,
	};
	rb_protect(IO_Event_Selector_blocking_operation_fallback, (VALUE)&arguments, &state);
#endif
	
	selector->blocked = 0;
	
	if (state) rb_jump_tag(state);
}

#ifndef HAVE_RB_IO_DESCRIPTOR
static ID id_fileno;

int IO_Event_Selector_io_descriptor(VALUE io) {
	return RB_NUM2INT(rb_funcall(io, id_fileno, 0));
}
#endif

#ifndef HAVE_RB_PROCESS_STATUS_WAIT
static ID id_wait;
static VALUE rb_Process_Status = Qnil;

VALUE IO_Event_Selector_process_status_wait(rb_pid_t pid, int flags)
{
	return rb_funcall(rb_Process_Status, id_wait, 2, PIDT2NUM(pid), INT2NUM(flags));
}
#endif

int IO_Event_Selector_nonblock_set(int file_descriptor)
{
#ifdef _WIN32
	return rb_w32_set_nonblock(file_descriptor);
#else
	// Get the current mode:
	int flags = fcntl(file_descriptor, F_GETFL, 0);
	
	// Set the non-blocking flag if it isn't already:
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags | O_NONBLOCK);
	}
	
	return flags;
#endif
}

void IO_Event_Selector_nonblock_restore(int file_descriptor, int flags)
{
#ifdef _WIN32
	// Yolo...
#else
	// The flags didn't have O_NONBLOCK set, so it would have been set, so we need to restore it:
	if (!(flags & O_NONBLOCK)) {
		fcntl(file_descriptor, F_SETFL, flags);
	}
#endif
}

struct IO_Event_Selector_nonblock_arguments {
	int file_descriptor;
	int flags;
};

static VALUE IO_Event_Selector_nonblock_ensure(VALUE _arguments) {
	struct IO_Event_Selector_nonblock_arguments *arguments = (struct IO_Event_Selector_nonblock_arguments *)_arguments;
	
	IO_Event_Selector_nonblock_restore(arguments->file_descriptor, arguments->flags);
	
	return Qnil;
}

static VALUE IO_Event_Selector_nonblock(VALUE class, VALUE io)
{
	struct IO_Event_Selector_nonblock_arguments arguments = {
		.file_descriptor = IO_Event_Selector_io_descriptor(io),
		.flags = IO_Event_Selector_nonblock_set(arguments.file_descriptor)
	};
	
	return rb_ensure(rb_yield, io, IO_Event_Selector_nonblock_ensure, (VALUE)&arguments);
}

static VALUE rb_IO_Event_Selector = Qnil;
static ID id_process_wait;

// Wait for a process when the selector cannot do so natively (e.g. `pid <= 0`: any child, or a process group). Delegates to the pure-Ruby `IO::Event::Selector.process_wait`, which performs a blocking wait on a separate thread; joining it is fiber-scheduler aware, so the reactor keeps running.
VALUE IO_Event_Selector_process_wait(rb_pid_t pid, int flags) {
	return rb_funcall(rb_IO_Event_Selector, id_process_wait, 2, PIDT2NUM(pid), INT2NUM(flags));
}

void Init_IO_Event_Selector(VALUE IO_Event_Selector) {
#ifndef RB_NOGVL_PENDING_INTR_FAIL
	handle_interrupt_id = rb_intern("handle_interrupt");
	signal_exception_never = rb_hash_new();
	rb_hash_aset(signal_exception_never, rb_eSignal, ID2SYM(rb_intern("never")));
	rb_funcall(signal_exception_never, rb_intern("compare_by_identity"), 0);
	rb_obj_freeze(signal_exception_never);
	rb_gc_register_mark_object(signal_exception_never);
#endif
	
	rb_IO_Event_Selector = IO_Event_Selector;
	rb_gc_register_mark_object(rb_IO_Event_Selector);
	id_process_wait = rb_intern("process_wait");
	
#ifndef HAVE_RB_IO_DESCRIPTOR
	id_fileno = rb_intern("fileno");
#endif
	
#ifndef HAVE_RB_PROCESS_STATUS_WAIT
	id_wait = rb_intern("wait");
	rb_Process_Status = rb_const_get_at(rb_mProcess, rb_intern("Status"));
	rb_gc_register_mark_object(rb_Process_Status);
#endif
	
	rb_define_singleton_method(IO_Event_Selector, "nonblock", IO_Event_Selector_nonblock, 1);
}

void IO_Event_Selector_initialize(struct IO_Event_Selector *backend, VALUE self, VALUE loop) {
	RB_OBJ_WRITE(self, &backend->self, self);
	RB_OBJ_WRITE(self, &backend->loop, loop);
	
	backend->waiting = NULL;
	backend->ready = NULL;
	backend->blocked = 0;
}

VALUE IO_Event_Selector_loop_resume(struct IO_Event_Selector *backend, VALUE fiber, int argc, VALUE *argv) {
	return IO_Event_Fiber_transfer(fiber, argc, argv);
}

VALUE IO_Event_Selector_loop_yield(struct IO_Event_Selector *backend)
{
	// Under normal operation, a user fiber yields back to the event loop fiber.
	// However, in some cases (e.g. blocking IO called from within the scheduler
	// fiber itself), the current fiber may already be the loop fiber. In that case,
	// transferring to ourselves would be a no-op in Ruby, but it signals a misuse:
	// the event loop fiber should never need to yield to itself, as nothing else
	// would be running to resume it. We return immediately rather than self-transferring.
	if (backend->loop == IO_Event_Fiber_current()) {
		// Uncomment to investigate the callsite that triggers this condition:
		// rb_warning("IO_Event_Selector_loop_yield: current fiber is the loop fiber");
		// rb_funcall(rb_mKernel, rb_intern("puts"), 1, rb_funcall(rb_cThread, rb_intern("current"), 0));
		return Qnil;
	}

	return IO_Event_Fiber_transfer(backend->loop, 0, NULL);
}

struct wait_and_transfer_arguments {
	int argc;
	VALUE *argv;
	
	struct IO_Event_Selector *backend;
	struct IO_Event_Selector_Queue *waiting;
};

static void queue_pop(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *waiting) {
	if (waiting->head) {
		waiting->head->tail = waiting->tail;
	} else {
		// We must have been at the head of the queue:
		backend->waiting = waiting->tail;
	}
	
	if (waiting->tail) {
		waiting->tail->head = waiting->head;
	} else {
		// We must have been at the tail of the queue:
		backend->ready = waiting->head;
	}
	
	waiting->head = NULL;
	waiting->tail = NULL;
}

static void queue_push(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *waiting) {
	assert(waiting->head == NULL);
	assert(waiting->tail == NULL);
	
	if (backend->waiting) {
		// If there was an item in the queue already, we shift it along:
		backend->waiting->head = waiting;
		waiting->tail = backend->waiting;
	} else {
		// If the queue was empty, we update the tail too:
		backend->ready = waiting;
	}
	
	// We always push to the front/head:
	backend->waiting = waiting;
}

static VALUE wait_and_transfer(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return IO_Event_Selector_loop_resume(arguments->backend, fiber, argc, argv);
}

static VALUE wait_and_transfer_ensure(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	queue_pop(arguments->backend, arguments->waiting);
	
	return Qnil;
}

VALUE IO_Event_Selector_resume(struct IO_Event_Selector *backend, int argc, VALUE *argv)
{
	rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);
	
	struct IO_Event_Selector_Queue waiting = {
		.head = NULL,
		.tail = NULL,
		.flags = IO_EVENT_SELECTOR_QUEUE_FIBER,
		.fiber = IO_Event_Fiber_current()
	};
	
	RB_OBJ_WRITTEN(backend->self, Qundef, waiting.fiber);
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_transfer, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

static VALUE wait_and_raise(VALUE _arguments) {
	struct wait_and_transfer_arguments *arguments = (struct wait_and_transfer_arguments *)_arguments;
	
	VALUE fiber = arguments->argv[0];
	int argc = arguments->argc - 1;
	VALUE *argv = arguments->argv + 1;
	
	return IO_Event_Fiber_raise(fiber, argc, argv);
}

VALUE IO_Event_Selector_raise(struct IO_Event_Selector *backend, int argc, VALUE *argv)
{
	rb_check_arity(argc, 2, UNLIMITED_ARGUMENTS);
	
	struct IO_Event_Selector_Queue waiting = {
		.head = NULL,
		.tail = NULL,
		.flags = IO_EVENT_SELECTOR_QUEUE_FIBER,
		.fiber = IO_Event_Fiber_current()
	};
	
	RB_OBJ_WRITTEN(backend->self, Qundef, waiting.fiber);
	
	queue_push(backend, &waiting);
	
	struct wait_and_transfer_arguments arguments = {
		.argc = argc,
		.argv = argv,
		.backend = backend,
		.waiting = &waiting,
	};
	
	return rb_ensure(wait_and_raise, (VALUE)&arguments, wait_and_transfer_ensure, (VALUE)&arguments);
}

void IO_Event_Selector_ready_push(struct IO_Event_Selector *backend, VALUE fiber)
{
	// Ruby's allocator triggers GC on memory pressure and raises `NoMemoryError` on failure, so no NULL check is required.
	struct IO_Event_Selector_Queue *waiting = xmalloc(sizeof(struct IO_Event_Selector_Queue));
	
	waiting->head = NULL;
	waiting->tail = NULL;
	waiting->flags = IO_EVENT_SELECTOR_QUEUE_INTERNAL;
	
	RB_OBJ_WRITE(backend->self, &waiting->fiber, fiber);
	
	queue_push(backend, waiting);
}

static inline
void IO_Event_Selector_ready_pop(struct IO_Event_Selector *backend, struct IO_Event_Selector_Queue *ready)
{
	if (DEBUG) fprintf(stderr, "IO_Event_Selector_ready_pop -> %p\n", (void*)ready->fiber);
	
	VALUE fiber = ready->fiber;
	
	if (ready->flags & IO_EVENT_SELECTOR_QUEUE_INTERNAL) {
		// This means that the fiber was added to the ready queue by the selector itself, and we need to transfer control to it, but before we do that, we need to remove it from the queue, as there is no expectation that returning from `transfer` will remove it.
		queue_pop(backend, ready);
		xfree(ready);
	} else if (ready->flags & IO_EVENT_SELECTOR_QUEUE_FIBER) {
		// This means the fiber added itself to the ready queue, and we need to transfer control back to it. Transferring control back to the fiber will call `queue_pop` and remove it from the queue.
	} else {
		rb_raise(rb_eRuntimeError, "Unknown queue type!");
	}
	
	IO_Event_Selector_loop_resume(backend, fiber, 0, NULL);
}

// State shared by the rb_ensure body and cleanup callbacks. Each flush owns a
// distinct placeholder on its C stack, including when flushes are nested.
struct ready_flush_arguments {
	struct IO_Event_Selector *backend;
	struct IO_Event_Selector_Queue placeholder;
	int count;
};

// rb_ensure requires VALUE(VALUE) callbacks. IO_Event_Selector_ready_flush wraps
// this loop and returns the count stored in the shared arguments.
static VALUE IO_Event_Selector_ready_flush_begin(VALUE _arguments)
{
	struct ready_flush_arguments *arguments = (struct ready_flush_arguments *)_arguments;
	struct IO_Event_Selector *backend = arguments->backend;
	
	while (backend->ready) {
		struct IO_Event_Selector_Queue *ready = backend->ready;
		
		// Each flush stops at its own placeholder. Entries beyond an outer
		// placeholder may already have been queued when this flush started.
		// Skip other placeholders without unlinking them: their owners still
		// need them as boundaries and will remove them in their ensure callbacks.
		while (ready && ready != &arguments->placeholder && (ready->flags & IO_EVENT_SELECTOR_QUEUE_PLACEHOLDER)) {
			ready = ready->head;
		}
		
		if (!ready || ready == &arguments->placeholder) break;
		
		// Resuming a fiber can unlink other entries, so read backend->ready again
		// on the next iteration instead of retaining a neighbour pointer.
		arguments->count += 1;
		IO_Event_Selector_ready_pop(backend, ready);
	}
	
	return Qnil;
}

static VALUE IO_Event_Selector_ready_flush_ensure(VALUE _arguments)
{
	struct ready_flush_arguments *arguments = (struct ready_flush_arguments *)_arguments;
	
	// Only the owning flush removes this placeholder, exactly once. Unlinking
	// it reconnects its neighbours without removing any other placeholders.
	queue_pop(arguments->backend, &arguments->placeholder);
	
	return Qnil;
}

int IO_Event_Selector_ready_flush(struct IO_Event_Selector *backend)
{
	if (!backend->ready) return 0;
	
	struct ready_flush_arguments arguments = {
		.backend = backend,
		.placeholder = {
			.head = NULL,
			.tail = NULL,
			.flags = IO_EVENT_SELECTOR_QUEUE_PLACEHOLDER,
			// Queue marking and compaction visit every node, including this one.
			.fiber = Qnil,
		},
		.count = 0,
	};
	
	// Saving the last existing entry is unsafe: another fiber can remove it.
	// Counting entries instead can let newly queued work replace removed entries.
	// This placeholder stays linked until its owning flush finishes, preserving:
	//   existing entries -> placeholder -> newly queued entries
	queue_push(backend, &arguments.placeholder);
	
	// Always unlink the placeholder on normal return or exception, before the
	// arguments leave scope, so the queue cannot retain a pointer into this stack.
	rb_ensure(IO_Event_Selector_ready_flush_begin, (VALUE)&arguments, IO_Event_Selector_ready_flush_ensure, (VALUE)&arguments);
	
	return arguments.count;
}
