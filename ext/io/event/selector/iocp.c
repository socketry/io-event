// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "iocp.h"
#include "selector.h"
#include "../list.h"
#include "../array.h"
#include "../time.h"

#ifdef _WIN32

// ruby.h → ruby/win32.h already includes winsock2.h and sets up the Win32
// environment correctly.  We include the headers below for their type and
// function declarations; they are no-ops at the preprocessor level because
// their include guards have already been set by ruby/win32.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stddef.h>
#include <time.h>

enum {
	DEBUG = 0,
	IOCP_MAX_EVENTS = 64,
};

// NTSTATUS codes stored in OVERLAPPED_ENTRY.Internal after a completion.
// Defined here to avoid pulling in <ntstatus.h> which conflicts with
// <windows.h> unless UMDF_USING_NTSTATUS is set.
#define IOCP_STATUS_SUCCESS         ((ULONG_PTR)0x00000000L)
#define IOCP_STATUS_CANCELLED       ((ULONG_PTR)0xC0000120L)
#define IOCP_STATUS_END_OF_FILE     ((ULONG_PTR)0xC0000011L)
#define IOCP_STATUS_PIPE_BROKEN     ((ULONG_PTR)0xC000014BL)
#define IOCP_STATUS_CONNECTION_RESET ((ULONG_PTR)0xC000020DL)

// Completion keys distinguish completion types in GetQueuedCompletionStatusEx.
// Wakeup uses a NULL overlapped pointer — no key value needed for that case.
#define IOCP_KEY_IO       ((ULONG_PTR)1)  // Normal overlapped I/O
#define IOCP_KEY_NOTIFY   ((ULONG_PTR)2)  // RegisterWaitForSingleObject-based

// ─── Data structures ─────────────────────────────────────────────────────────

enum IO_Event_Selector_IOCP_Completion_State {
	IOCP_COMPLETION_IDLE = 0,
	IOCP_COMPLETION_SUBMITTED,
	IOCP_COMPLETION_DETACHED,
};

struct IO_Event_Selector_IOCP_Notify;

// Pool entry.  OVERLAPPED *must* be the first field so that we can cast
// between (LPOVERLAPPED) and (struct IO_Event_Selector_IOCP_Completion *).
// This is the pointer that travels through the IOCP queue; the stack-allocated
// Waiting struct is never referenced from the queue, giving safe cancellation.
struct IO_Event_Selector_IOCP_Completion {
	OVERLAPPED overlapped;    // ← must be first
	struct IO_Event_List list;

	// Back-pointer to the stack-allocated Waiting.  Nulled by completion_detach()
	// before the stack frame is released, so process_completions never chases
	// a dangling pointer.
	struct IO_Event_Selector_IOCP_Waiting *waiting;

	// Notify state for RegisterWaitForSingleObject-based completions. Present
	// only for IOCP_KEY_NOTIFY operations and always freed by
	// process_completions, never by ensure.
	struct IO_Event_Selector_IOCP_Notify *notify;

	// Once an operation has been submitted, the completion slot and its
	// OVERLAPPED storage are owned by the kernel or a notification callback
	// until process_completions receives the queued completion.
	enum IO_Event_Selector_IOCP_Completion_State state;

	// For io_wait operations, any non-cancelled completion means the requested
	// readiness was observed, including EOF/hangup style completions.
	int readiness;
};

// Stack-allocated per-operation state.  Lives in the suspended fiber's stack
// for the duration of the operation.
struct IO_Event_Selector_IOCP_Waiting {
	struct IO_Event_Selector_IOCP_Completion *completion;

	VALUE  fiber;
	int    result;   // bytes transferred (≥0), or negated errno (<0)
	HANDLE handle;   // file/socket handle, for CancelIoEx
};

// Per-operation state for RegisterWaitForSingleObject-based waits
// (process_wait and io_wait-writable). Heap-allocated; always freed in
// process_completions so it outlives any cancellation in ensure.
struct IO_Event_Selector_IOCP_Notify {
	HANDLE           port;        // back-pointer to the IOCP
	OVERLAPPED      *overlapped;  // points to completion->overlapped
	volatile LONG    posted;      // 1 once PostQueuedCompletionStatus is called
	HANDLE           process;     // non-NULL for process_wait
	WSAEVENT         wsa_event;   // non-NULL for io_wait-writable
	SOCKET           socket;      // socket associated with wsa_event
	HANDLE           wait_handle; // from RegisterWaitForSingleObject
};

struct IO_Event_Selector_IOCP {
	struct IO_Event_Selector backend;

	HANDLE port;      // I/O Completion Port
	volatile LONG selecting; // 1 while a select call is active.
	volatile LONG blocked;   // 1 while sleeping in GetQueuedCompletionStatusEx.
	volatile LONG wakeup_pending;

	struct timespec idle_duration;

	struct IO_Event_Array completions; // pool of IO_Event_Selector_IOCP_Completion
	struct IO_Event_List  free_list;
	int submitted_count;
	int detached_count;

	// Reusable dequeue buffer — kept here (in heap-allocated TypedData) rather
	// than on the stack to avoid triggering -fstack-protector-strong, whose
	// __stack_chk_fail / __stack_chk_guard symbols may be unresolvable when
	// linking a shared DLL on some MinGW-w64 configurations.
	OVERLAPPED_ENTRY completion_entries[IOCP_MAX_EVENTS];
};

static struct IO_Event_Selector_IOCP_Completion *
completion_from_list(struct IO_Event_List *list)
{
	return (struct IO_Event_Selector_IOCP_Completion *)((char *)list -
	    offsetof(struct IO_Event_Selector_IOCP_Completion, list));
}

static void
completion_cancel(struct IO_Event_Selector_IOCP *selector,
                  struct IO_Event_Selector_IOCP_Waiting *waiting);

// ─── GC support ──────────────────────────────────────────────────────────────

static void
IO_Event_Selector_IOCP_Completion_mark(void *_completion)
{
	struct IO_Event_Selector_IOCP_Completion *c = _completion;
	if (c->waiting && c->waiting->fiber)
		rb_gc_mark_movable(c->waiting->fiber);
}

static void
IO_Event_Selector_IOCP_Type_mark(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	IO_Event_Selector_mark(&selector->backend);
	IO_Event_Array_each(&selector->completions,
	                    IO_Event_Selector_IOCP_Completion_mark);
}

static void
IO_Event_Selector_IOCP_Completion_compact(void *_completion)
{
	struct IO_Event_Selector_IOCP_Completion *c = _completion;
	if (c->waiting && c->waiting->fiber)
		c->waiting->fiber = rb_gc_location(c->waiting->fiber);
}

static void
IO_Event_Selector_IOCP_Type_compact(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	IO_Event_Selector_compact(&selector->backend);
	IO_Event_Array_each(&selector->completions,
	                    IO_Event_Selector_IOCP_Completion_compact);
}

static int
process_completions(struct IO_Event_Selector_IOCP *selector, DWORD timeout_ms);

static void
drain_detached_completions(struct IO_Event_Selector_IOCP *selector)
{
	int attempts = 100;

	while (selector->detached_count > 0 && attempts-- > 0) {
		process_completions(selector, 10);
	}

	if (selector->detached_count > 0) {
		rb_warn("IOCP selector closed with %d detached completion(s) pending",
		        selector->detached_count);
	}
}

static void
close_internal(struct IO_Event_Selector_IOCP *selector, int drain)
{
	if (selector->port && selector->port != INVALID_HANDLE_VALUE) {
		if (drain)
			drain_detached_completions(selector);

		CloseHandle(selector->port);
		selector->port = INVALID_HANDLE_VALUE;
	}
}

static void
IO_Event_Selector_IOCP_Type_free(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	close_internal(selector, 0);
	IO_Event_Array_free(&selector->completions);
	xfree(selector);
}

static size_t
IO_Event_Selector_IOCP_Type_size(const void *_selector)
{
	const struct IO_Event_Selector_IOCP *selector = _selector;
	return sizeof(*selector)
	     + IO_Event_Array_memory_size(&selector->completions);
}

static const rb_data_type_t IO_Event_Selector_IOCP_Type = {
	.wrap_struct_name = "IO::Event::Selector::IOCP",
	.function = {
		.dmark   = IO_Event_Selector_IOCP_Type_mark,
		.dcompact = IO_Event_Selector_IOCP_Type_compact,
		.dfree   = IO_Event_Selector_IOCP_Type_free,
		.dsize   = IO_Event_Selector_IOCP_Type_size,
	},
	.flags = RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED,
};

// ─── Completion pool ─────────────────────────────────────────────────────────

static void
IO_Event_Selector_IOCP_Completion_initialize(void *element)
{
	struct IO_Event_Selector_IOCP_Completion *c = element;
	memset(&c->overlapped, 0, sizeof(c->overlapped));
	IO_Event_List_clear(&c->list);
	c->waiting = NULL;
	c->notify  = NULL;
	c->state   = IOCP_COMPLETION_IDLE;
	c->readiness = 0;
}

static void
IO_Event_Selector_IOCP_Completion_free_element(void *element)
{
	struct IO_Event_Selector_IOCP_Completion *c = element;
	if (c->waiting) {
		c->waiting->completion = NULL;
		c->waiting = NULL;
	}
}

static struct IO_Event_Selector_IOCP_Completion *
completion_acquire(struct IO_Event_Selector_IOCP *selector,
                   struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	struct IO_Event_Selector_IOCP_Completion *c;

	if (!IO_Event_List_empty(&selector->free_list)) {
		c = completion_from_list(selector->free_list.tail);
		IO_Event_List_pop(&c->list);
		if (c->state != IOCP_COMPLETION_IDLE || c->waiting)
			rb_bug("IOCP completion acquired while still in use");
	} else {
		c = IO_Event_Array_push(&selector->completions);
		IO_Event_List_clear(&c->list);
	}

	memset(&c->overlapped, 0, sizeof(c->overlapped));
	c->waiting = waiting;
	c->notify  = NULL;
	c->state   = IOCP_COMPLETION_IDLE;
	c->readiness = 0;
	waiting->completion = c;

	return c;
}

static void
completion_submit(struct IO_Event_Selector_IOCP *selector,
                  struct IO_Event_Selector_IOCP_Completion *c)
{
	// Once submitted, the OVERLAPPED storage belongs to the kernel or a
	// notification callback until the queued completion is processed.
	if (c->state == IOCP_COMPLETION_DETACHED)
		rb_bug("IOCP detached completion submitted again");

	if (c->state == IOCP_COMPLETION_IDLE) {
		c->state = IOCP_COMPLETION_SUBMITTED;
		selector->submitted_count += 1;
	}
}

static void
completion_release(struct IO_Event_Selector_IOCP *selector,
                   struct IO_Event_Selector_IOCP_Completion *c)
{
	if (c->waiting && c->waiting->completion == c)
		c->waiting->completion = NULL;

	switch (c->state) {
	case IOCP_COMPLETION_IDLE:
		break;
	case IOCP_COMPLETION_SUBMITTED:
		if (selector->submitted_count <= 0)
			rb_bug("IOCP submitted completion count underflow");
		selector->submitted_count -= 1;
		break;
	case IOCP_COMPLETION_DETACHED:
		if (selector->detached_count <= 0)
			rb_bug("IOCP detached completion count underflow");
		selector->detached_count -= 1;
		break;
	}

	c->waiting = NULL;
	c->notify  = NULL;
	c->state   = IOCP_COMPLETION_IDLE;
	c->readiness = 0;
	IO_Event_List_prepend(&selector->free_list, &c->list);
}

static void
completion_detach(struct IO_Event_Selector_IOCP *selector,
                  struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	if (waiting->completion) {
		struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;

		// The waiting object lives on the suspended fiber's stack. Detaching
		// clears the back-pointer before that stack frame can unwind; any late
		// completion will simply release the slot without resuming a fiber.
		c->waiting = NULL;
		if (c->state == IOCP_COMPLETION_SUBMITTED) {
			if (selector->submitted_count <= 0)
				rb_bug("IOCP submitted completion count underflow");
			selector->submitted_count -= 1;
			c->state = IOCP_COMPLETION_DETACHED;
			selector->detached_count += 1;
		}
		waiting->completion = NULL;
	}
	waiting->fiber = 0;
}

static void
completion_drain(struct IO_Event_Selector_IOCP *selector,
                 struct IO_Event_Selector_IOCP_Completion *c)
{
	int attempts = 100;

	while (c->state != IOCP_COMPLETION_IDLE && attempts-- > 0) {
		process_completions(selector, 10);
	}

	if (c->state != IOCP_COMPLETION_IDLE) {
		rb_warn("IOCP completion cancellation did not drain before reuse");
	}
}

static void
completion_cancel_notify(struct IO_Event_Selector_IOCP *selector,
                         struct IO_Event_Selector_IOCP_Completion *c)
{
	struct IO_Event_Selector_IOCP_Notify *notify = c->notify;

	c->waiting = NULL;

	// Ensure exactly one completion arrives so process_completions can free
	// the notify struct and release the completion slot.
	if (notify && InterlockedCompareExchange(&notify->posted, 1, 0) == 0) {
		PostQueuedCompletionStatus(selector->port, 0,
		                           IOCP_KEY_NOTIFY, &c->overlapped);
	}

	// Non-blocking unregister — the callback may have already fired.
	if (notify && notify->wait_handle)
		UnregisterWait(notify->wait_handle);
}

static void
completion_cancel_overlapped(struct IO_Event_Selector_IOCP_Waiting *waiting,
                             struct IO_Event_Selector_IOCP_Completion *c)
{
	c->waiting = NULL;
	CancelIoEx(waiting->handle, &c->overlapped);
}

static void
completion_cancel_submitted(struct IO_Event_Selector_IOCP *selector,
                            struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;

	if (c) {
		if (c->state == IOCP_COMPLETION_IDLE) {
			completion_cancel(selector, waiting);
			return;
		} else if (c->notify) {
			completion_cancel_notify(selector, c);
		} else {
			completion_cancel_overlapped(waiting, c);
		}
	}

	completion_detach(selector, waiting);
	if (c)
		completion_drain(selector, c);
}

static void
completion_cancel(struct IO_Event_Selector_IOCP *selector,
                  struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;
	if (c)
		completion_release(selector, c);
	waiting->completion = NULL;
	waiting->fiber = 0;
}

// ─── Handle helpers ───────────────────────────────────────────────────────────

// Lazily associate a handle with our IOCP. Windows reports
// ERROR_INVALID_PARAMETER when a handle is already associated with a completion
// port; it does not give us a useful way to distinguish "already ours" from
// "owned by another port" here. In normal use Ruby-created sockets are not
// shared with another IOCP, so we treat that case as already associated. If a
// handle is associated with another IOCP, behaviour is outside this selector's
// supported contract.
static void
ensure_associated(struct IO_Event_Selector_IOCP *selector, HANDLE h)
{
	HANDLE result = CreateIoCompletionPort(h, selector->port, IOCP_KEY_IO, 0);
	if (result == NULL) {
		DWORD err = GetLastError();
		if (err != ERROR_INVALID_PARAMETER)
			rb_syserr_fail((int)rb_w32_map_errno(err),
			               "ensure_associated:CreateIoCompletionPort");
	}
}

static void
socket_event_select_cancel(SOCKET socket)
{
	if (socket != INVALID_SOCKET)
		WSAEventSelect(socket, NULL, 0);
}

static void
notify_close_wsa_event(struct IO_Event_Selector_IOCP_Notify *notify)
{
	WSAEVENT wsa_event = (WSAEVENT)InterlockedExchangePointer(
	    (PVOID volatile *)&notify->wsa_event, NULL);

	if (wsa_event) {
		if (notify->socket != INVALID_SOCKET) {
			socket_event_select_cancel(notify->socket);
			notify->socket = INVALID_SOCKET;
		}

		WSACloseEvent(wsa_event);
	}
}

static int
socket_in_set(fd_set *set, SOCKET socket)
{
	for (u_int i = 0; i < set->fd_count; i++) {
		if (set->fd_array[i] == socket)
			return 1;
	}

	return 0;
}

static int
socket_ready_select(SOCKET socket, int requested)
{
	fd_set read_fds, write_fds, except_fds;
	fd_set *read_ptr = NULL, *write_ptr = NULL;
	struct timeval timeout = {0, 0};

	except_fds.fd_count = 0;
	except_fds.fd_array[except_fds.fd_count++] = socket;

	if (requested & IO_EVENT_READABLE) {
		read_fds.fd_count = 0;
		read_fds.fd_array[read_fds.fd_count++] = socket;
		read_ptr = &read_fds;
	}

	if (requested & IO_EVENT_WRITABLE) {
		write_fds.fd_count = 0;
		write_fds.fd_array[write_fds.fd_count++] = socket;
		write_ptr = &write_fds;
	}

	// Do not use FD_SET here: ruby/win32.h redefines it to work with CRT file
	// descriptors, while Winsock select() expects raw SOCKET handles.
	int rc = select(0, read_ptr, write_ptr, &except_fds, &timeout);
	if (rc == SOCKET_ERROR)
		return -WSAGetLastError();

	int ready = 0;
	if (read_ptr && socket_in_set(read_ptr, socket))
		ready |= IO_EVENT_READABLE;
	if (write_ptr && socket_in_set(write_ptr, socket))
		ready |= IO_EVENT_WRITABLE;
	if (socket_in_set(&except_fds, socket))
		ready |= requested;

	return ready & requested;
}

static int
handle_ready_immediately(HANDLE handle, int requested)
{
	DWORD type = GetFileType(handle);

	// Disk files do not have useful readiness semantics on Windows. Treat them
	// as ready and let the actual read/write operation report any real error.
	if (type == FILE_TYPE_DISK)
		return requested & (IO_EVENT_READABLE | IO_EVENT_WRITABLE);

	// Character devices are not IOCP-friendly in the general case. Reporting
	// readiness here matches the pragmatic file behavior above.
	if (type == FILE_TYPE_CHAR)
		return requested & (IO_EVENT_READABLE | IO_EVENT_WRITABLE);

	// There is no general non-socket writable readiness primitive for handles.
	// Pipes are commonly writable until the following WriteFile proves
	// otherwise, so keep this optimistic and avoid a fake wait.
	if (requested & IO_EVENT_WRITABLE)
		return IO_EVENT_WRITABLE;

	return 0;
}

static int
io_wait_ready_immediately(int is_socket, HANDLE handle, SOCKET socket,
                          int requested)
{
	if (is_socket && (requested & IO_EVENT_WRITABLE)) {
		int ready = socket_ready_select(socket, requested);
		if (ready != 0)
			return ready;
	}

	if (!is_socket)
		return handle_ready_immediately(handle, requested);

	return 0;
}

// ─── NTSTATUS → errno ────────────────────────────────────────────────────────

typedef ULONG (WINAPI *RtlNtStatusToDosError_fn)(ULONG);

static DWORD
ntstatus_to_win32(ULONG_PTR status)
{
	switch (status) {
	case IOCP_STATUS_SUCCESS:          return ERROR_SUCCESS;
	case IOCP_STATUS_CANCELLED:        return ERROR_OPERATION_ABORTED;
	case IOCP_STATUS_END_OF_FILE:      return ERROR_HANDLE_EOF;
	case IOCP_STATUS_PIPE_BROKEN:      return ERROR_BROKEN_PIPE;
	case IOCP_STATUS_CONNECTION_RESET: return ERROR_NETNAME_DELETED;
	default: {
		static RtlNtStatusToDosError_fn fn = NULL;
		if (!fn) {
			HMODULE ntdll = GetModuleHandleA("ntdll.dll");
			if (ntdll)
				fn = (RtlNtStatusToDosError_fn)
				     GetProcAddress(ntdll, "RtlNtStatusToDosError");
		}
		return fn ? fn((ULONG)status) : ERROR_UNIDENTIFIED_ERROR;
	}
	}
}

// Returns bytes (≥0) for success/EOF, or negated errno (<0) for errors.
// The caller checks for IOCP_STATUS_CANCELLED separately.
static int
iocp_result(ULONG_PTR status, DWORD bytes)
{
	if (status == IOCP_STATUS_SUCCESS)     return (int)bytes;
	if (status == IOCP_STATUS_END_OF_FILE) return 0; // EOF
	DWORD win32_err = ntstatus_to_win32(status);
	return -(int)rb_w32_map_errno(win32_err);
}

// ─── Allocate / initialize / close ───────────────────────────────────────────

VALUE
IO_Event_Selector_IOCP_allocate(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	VALUE instance = TypedData_Make_Struct(self,
	                     struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	IO_Event_Selector_initialize(&selector->backend, self, Qnil);
	selector->port    = INVALID_HANDLE_VALUE;
	selector->selecting = 0;
	selector->blocked = 0;
	selector->wakeup_pending = 0;
	selector->submitted_count = 0;
	selector->detached_count = 0;

	IO_Event_List_initialize(&selector->free_list);

	selector->completions.element_initialize =
	    IO_Event_Selector_IOCP_Completion_initialize;
	selector->completions.element_free =
	    IO_Event_Selector_IOCP_Completion_free_element;

	IO_Event_Array_initialize(&selector->completions,
	                          IO_EVENT_ARRAY_DEFAULT_COUNT,
	                          sizeof(struct IO_Event_Selector_IOCP_Completion));

	return instance;
}

VALUE
IO_Event_Selector_IOCP_initialize(VALUE self, VALUE loop)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	IO_Event_Selector_initialize(&selector->backend, self, loop);

	// Create the completion port (no file handle yet).
	selector->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!selector->port || selector->port == INVALID_HANDLE_VALUE)
		rb_sys_fail("IO_Event_Selector_IOCP_initialize:CreateIoCompletionPort");

	return self;
}

VALUE
IO_Event_Selector_IOCP_loop(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return selector->backend.loop;
}

VALUE
IO_Event_Selector_IOCP_idle_duration(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	double d = selector->idle_duration.tv_sec
	         + selector->idle_duration.tv_nsec / 1e9;
	return DBL2NUM(d);
}

VALUE
IO_Event_Selector_IOCP_close(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	close_internal(selector, 1);
	return Qnil;
}

// ─── Scheduling primitives (delegated to selector.c) ─────────────────────────

VALUE IO_Event_Selector_IOCP_transfer(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return IO_Event_Selector_loop_yield(&selector->backend);
}

VALUE IO_Event_Selector_IOCP_resume(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return IO_Event_Selector_resume(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_IOCP_yield(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return IO_Event_Selector_yield(&selector->backend);
}

VALUE IO_Event_Selector_IOCP_push(VALUE self, VALUE fiber)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	IO_Event_Selector_ready_push(&selector->backend, fiber);
	return Qnil;
}

VALUE IO_Event_Selector_IOCP_raise(int argc, VALUE *argv, VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return IO_Event_Selector_raise(&selector->backend, argc, argv);
}

VALUE IO_Event_Selector_IOCP_ready_p(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);
	return selector->backend.ready ? Qtrue : Qfalse;
}

// ─── process_wait ────────────────────────────────────────────────────────────

// Called from the OS thread-pool when a waited process exits.
static VOID CALLBACK
process_exit_callback(PVOID param, BOOLEAN timer)
{
	(void)timer;
	struct IO_Event_Selector_IOCP_Notify *notify = param;

	// Exactly one PostQueuedCompletionStatus, even if both the callback and
	// the cancellation path try simultaneously.
	if (InterlockedCompareExchange(&notify->posted, 1, 0) == 0) {
		PostQueuedCompletionStatus(notify->port, 0,
		                           IOCP_KEY_NOTIFY, notify->overlapped);
	}
}

struct process_wait_arguments {
	struct IO_Event_Selector_IOCP         *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
	pid_t  pid;
	int    flags;
};

static VALUE
process_wait_transfer(VALUE _arguments)
{
	struct process_wait_arguments *args =
	    (struct process_wait_arguments *)_arguments;

	IO_Event_Selector_loop_yield(&args->selector->backend);

	if (args->waiting->result == 0)
		return IO_Event_Selector_process_status_wait(args->pid, args->flags);

	return Qfalse;
}

static VALUE
process_wait_ensure(VALUE _arguments)
{
	struct process_wait_arguments *args =
	    (struct process_wait_arguments *)_arguments;

	completion_cancel_submitted(args->selector, args->waiting);
	return Qnil;
}

VALUE
IO_Event_Selector_IOCP_process_wait(VALUE self, VALUE fiber,
                                    VALUE _pid, VALUE _flags)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	pid_t pid   = NUM2PIDT(_pid);
	int   flags = NUM2INT(_flags);

	HANDLE process = OpenProcess(
	    SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
	if (!process)
		rb_sys_fail("IO_Event_Selector_IOCP_process_wait:OpenProcess");

	struct IO_Event_Selector_IOCP_Waiting waiting = {.fiber = fiber};
	RB_OBJ_WRITTEN(self, Qundef, fiber);

	struct IO_Event_Selector_IOCP_Completion *c =
	    completion_acquire(selector, &waiting);

	struct IO_Event_Selector_IOCP_Notify *notify =
	    malloc(sizeof(struct IO_Event_Selector_IOCP_Notify));
	if (!notify) {
		CloseHandle(process);
		completion_cancel(selector, &waiting);
		rb_memerror();
	}
	notify->port        = selector->port;
	notify->overlapped  = &c->overlapped;
	notify->posted      = 0;
	notify->process     = process;
	notify->wsa_event   = NULL;
	notify->socket      = INVALID_SOCKET;
	notify->wait_handle = NULL;
	c->notify = notify;

	completion_submit(selector, c);

	if (!RegisterWaitForSingleObject(&notify->wait_handle, process,
	                                 process_exit_callback, notify,
	                                 INFINITE, WT_EXECUTEONLYONCE)) {
		free(notify);
		c->notify = NULL;
		CloseHandle(process);
		completion_cancel(selector, &waiting);
		rb_sys_fail("IO_Event_Selector_IOCP_process_wait:"
		            "RegisterWaitForSingleObject");
	}

	struct process_wait_arguments args = {
		.selector = selector,
		.waiting  = &waiting,
		.pid      = pid,
		.flags    = flags,
	};

	return rb_ensure(process_wait_transfer, (VALUE)&args,
	                 process_wait_ensure,   (VALUE)&args);
}

// ─── io_wait ─────────────────────────────────────────────────────────────────

struct io_wait_arguments {
	struct IO_Event_Selector_IOCP         *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
};

static VALUE
io_wait_ensure(VALUE _arguments)
{
	struct io_wait_arguments *args =
	    (struct io_wait_arguments *)_arguments;

	completion_cancel_submitted(args->selector, args->waiting);
	return Qnil;
}

static VALUE
io_wait_transfer(VALUE _arguments)
{
	struct io_wait_arguments *args =
	    (struct io_wait_arguments *)_arguments;

	IO_Event_Selector_loop_yield(&args->selector->backend);

	int result = args->waiting->result;
	if (result >= 0)
		return RB_INT2NUM(result);   // IO_EVENT_READABLE or IO_EVENT_WRITABLE
	return Qfalse;
}

// Callback for WSAEventSelect-based writable wait.
static VOID CALLBACK
io_wait_writable_callback(PVOID param, BOOLEAN timer)
{
	(void)timer;
	struct IO_Event_Selector_IOCP_Notify *notify = param;

	notify_close_wsa_event(notify);

	if (InterlockedCompareExchange(&notify->posted, 1, 0) == 0) {
		PostQueuedCompletionStatus(notify->port, IO_EVENT_WRITABLE,
		                           IOCP_KEY_NOTIFY, notify->overlapped);
	}
}

static VALUE
io_wait_ready(struct IO_Event_Selector_IOCP *selector, VALUE fiber, int events)
{
	IO_Event_Selector_ready_push(&selector->backend, fiber);
	IO_Event_Selector_loop_yield(&selector->backend);
	return RB_INT2NUM(events);
}

static void
io_wait_register_readable(struct IO_Event_Selector_IOCP *selector,
                          struct IO_Event_Selector_IOCP_Waiting *waiting,
                          struct IO_Event_Selector_IOCP_Completion *c,
                          int is_socket, HANDLE handle, SOCKET socket)
{
	// Zero-byte overlapped read: completes when data is available.
	// Works for sockets (WSARecv) and overlapped-capable non-disk handles.
	// Disk files are handled by handle_ready_immediately because Windows does
	// not expose useful readiness semantics for them.
	c->readiness = IO_EVENT_READABLE;

	ensure_associated(selector, handle);
	completion_submit(selector, c);

	if (is_socket) {
		WSABUF wsabuf = {0, NULL};
		DWORD  bytes = 0, wsa_flags = 0;
		int rc = WSARecv(socket, &wsabuf, 1, &bytes, &wsa_flags,
		                 &c->overlapped, NULL);
		if (rc == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING) {
				completion_cancel(selector, waiting);
				rb_syserr_fail(rb_w32_map_errno(err),
				               "io_wait:WSARecv");
			}
		}
	} else {
		// Zero-byte ReadFile: pends until data is available on a pipe.
		DWORD bytes = 0;
		if (!ReadFile(handle, NULL, 0, &bytes, &c->overlapped)) {
			DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING) {
				completion_cancel(selector, waiting);
				rb_syserr_fail(rb_w32_map_errno(err),
				               "io_wait:ReadFile");
			}
		}
	}
}

static void
io_wait_register_writable_socket(struct IO_Event_Selector_IOCP *selector,
                                 struct IO_Event_Selector_IOCP_Waiting *waiting,
                                 struct IO_Event_Selector_IOCP_Completion *c,
                                 SOCKET socket)
{
	// Use WSAEventSelect + OS thread pool for writable detection.
	// FD_WRITE fires immediately for freshly-connected sockets and after
	// WSAEWOULDBLOCK on send; FD_CONNECT fires on async connect.
	// We avoid select()+FD_SET here because ruby/win32.h redefines FD_SET to
	// call _get_osfhandle(descriptor) expecting a CRT integer descriptor, not
	// a raw SOCKET handle, which would produce wrong results.
	WSAEVENT wsa_event = WSACreateEvent();
	if (wsa_event == WSA_INVALID_EVENT) {
		completion_cancel(selector, waiting);
		rb_syserr_fail(rb_w32_map_errno(WSAGetLastError()),
		               "io_wait:WSACreateEvent");
	}

	// FD_WRITE fires when previously-blocked send becomes possible;
	// FD_CONNECT fires when an async connect completes.
	if (WSAEventSelect(socket, wsa_event, FD_WRITE | FD_CONNECT) == SOCKET_ERROR) {
		WSACloseEvent(wsa_event);
		completion_cancel(selector, waiting);
		rb_syserr_fail(rb_w32_map_errno(WSAGetLastError()),
		               "io_wait:WSAEventSelect");
	}

	struct IO_Event_Selector_IOCP_Notify *notify = malloc(sizeof(*notify));
	if (!notify) {
		socket_event_select_cancel(socket);
		WSACloseEvent(wsa_event);
		completion_cancel(selector, waiting);
		rb_memerror();
	}
	notify->port        = selector->port;
	notify->overlapped  = &c->overlapped;
	notify->posted      = 0;
	notify->process     = NULL;
	notify->wsa_event   = wsa_event;
	notify->socket      = socket;
	notify->wait_handle = NULL;
	c->notify = notify;

	completion_submit(selector, c);

	if (!RegisterWaitForSingleObject(&notify->wait_handle, wsa_event,
	                                 io_wait_writable_callback,
	                                 notify, INFINITE,
	                                 WT_EXECUTEONLYONCE)) {
		socket_event_select_cancel(socket);
		WSACloseEvent(wsa_event);
		free(notify);
		c->notify = NULL;
		completion_cancel(selector, waiting);
		rb_sys_fail("io_wait:RegisterWaitForSingleObject");
	}
}

VALUE
IO_Event_Selector_IOCP_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	int descriptor      = IO_Event_Selector_io_descriptor(io);
	int requested       = RB_NUM2INT(events);
	HANDLE handle       = (HANDLE)rb_w32_get_osfhandle(descriptor);
	int    is_socket    = rb_w32_is_socket(descriptor);
	SOCKET socket       = (SOCKET)handle;

	// Windows has no single readiness primitive covering sockets, pipes, and
	// disk files. The IOCP selector is therefore socket-first:
	// - socket writable readiness is checked with Winsock select() before
	//   arming slower notification paths;
	// - disk and character handles are treated as immediately ready;
	// - non-socket writable waits are optimistic and the following operation
	//   reports any real error or backpressure.
	//
	// For mixed READABLE | WRITABLE waits, this also makes the policy explicit:
	// return any immediately-ready writable/error state first; otherwise arm
	// the readable wait below.
	int ready = io_wait_ready_immediately(is_socket, handle, socket, requested);
	if (ready > 0)
		return io_wait_ready(selector, fiber, ready);
	else if (ready < 0)
		rb_syserr_fail(rb_w32_map_errno(-ready), "io_wait:select");

	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.result = -EAGAIN,
		.handle = handle,
	};
	RB_OBJ_WRITTEN(self, Qundef, fiber);

	struct IO_Event_Selector_IOCP_Completion *c =
	    completion_acquire(selector, &waiting);

	struct io_wait_arguments args = {
		.selector = selector,
		.waiting  = &waiting,
	};

	if (requested & IO_EVENT_READABLE) {
		io_wait_register_readable(selector, &waiting, c,
		                          is_socket, handle, socket);
	} else if (requested & IO_EVENT_WRITABLE) {
		if (is_socket)
			io_wait_register_writable_socket(selector, &waiting, c, socket);
	}

	return rb_ensure(io_wait_transfer, (VALUE)&args,
	                 io_wait_ensure,   (VALUE)&args);
}

// ─── io_read / io_write ───────────────────────────────────────────────────────

#ifdef HAVE_RUBY_IO_BUFFER_H

struct io_read_arguments {
	struct IO_Event_Selector_IOCP         *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
};

static int
submit_read(struct IO_Event_Selector_IOCP *selector,
            struct IO_Event_Selector_IOCP_Waiting *waiting,
            int is_socket, HANDLE handle, SOCKET socket,
            void *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;

	memset(&c->overlapped, 0, sizeof(c->overlapped));

	if (is_socket) {
		WSABUF wsabuf = {(ULONG)len, (char *)base};
		DWORD  bytes = 0, flags = 0;
		completion_submit(selector, c);
		int rc = WSARecv(socket, &wsabuf, 1, &bytes, &flags,
		                 &c->overlapped, NULL);
		if (rc == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	} else {
		DWORD bytes = 0;
		completion_submit(selector, c);
		if (!ReadFile(handle, base, (DWORD)len, &bytes,
		              &c->overlapped)) {
			DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	}
	return 0; // pending
}

static VALUE
io_read_submit(VALUE _arguments)
{
	struct io_read_arguments *args = (struct io_read_arguments *)_arguments;

	IO_Event_Selector_loop_yield(&args->selector->backend);

	return RB_INT2NUM(args->waiting->result);
}

static VALUE
io_read_ensure(VALUE _arguments)
{
	struct io_read_arguments *args = (struct io_read_arguments *)_arguments;

	completion_cancel_submitted(args->selector, args->waiting);
	return Qnil;
}

static int
do_read(struct IO_Event_Selector_IOCP *selector,
        VALUE self, VALUE fiber, VALUE io,
        int is_socket, HANDLE handle, SOCKET socket,
        char *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.handle = handle,
	};
	RB_OBJ_WRITTEN(selector->backend.self, Qundef, fiber);

	completion_acquire(selector, &waiting);

	int submit_result = submit_read(selector, &waiting, is_socket, handle,
	                                socket, base, len);
	if (submit_result < 0) {
		completion_cancel(selector, &waiting);
		return submit_result;
	}

	struct io_read_arguments args = {
		.selector = selector,
		.waiting  = &waiting,
	};

	VALUE rv = rb_ensure(io_read_submit, (VALUE)&args,
	                     io_read_ensure,  (VALUE)&args);
	return RB_NUM2INT(rv);
}

VALUE
IO_Event_Selector_IOCP_io_read(VALUE self, VALUE fiber, VALUE io,
                                VALUE buffer, VALUE _length, VALUE _offset)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	int    descriptor = IO_Event_Selector_io_descriptor(io);
	HANDLE handle     = (HANDLE)rb_w32_get_osfhandle(descriptor);
	int    is_socket  = rb_w32_is_socket(descriptor);
	SOCKET socket     = (SOCKET)handle;

	ensure_associated(selector, handle);

	void  *base;
	size_t size;
	rb_io_buffer_get_bytes_for_writing(buffer, &base, &size);

	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	size_t total  = 0;

	if (offset > size)
		return rb_fiber_scheduler_io_result(-1, EINVAL);

	size_t maximum_size = size - offset;

	// length == 0 (non-blocking peek) is not specially handled here.
	// On IOCP, all reads go through overlapped completion; there is no
	// zero-copy "give me whatever is buffered" path.
	if (!length) length = maximum_size;
	while (maximum_size) {
		int result = do_read(selector, self, fiber, io,
		                     is_socket, handle, socket,
		                     (char *)base + offset, maximum_size);
		if (result > 0) {
			total  += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break; // EOF
		} else if (length > 0 && IO_Event_try_again(-result)) {
			IO_Event_Selector_IOCP_io_wait(self, fiber, io,
			                               RB_INT2NUM(IO_EVENT_READABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, -result);
		}
		maximum_size = size - offset;
	}

	return rb_fiber_scheduler_io_result((int)total, 0);
}

static VALUE IO_Event_Selector_IOCP_io_read_compatible(int argc, VALUE *argv,
                                                        VALUE self)
{
	rb_check_arity(argc, 4, 5);
	VALUE offset = (argc == 5) ? argv[4] : SIZET2NUM(0);
	return IO_Event_Selector_IOCP_io_read(self, argv[0], argv[1],
	                                       argv[2], argv[3], offset);
}

// ── io_write ─────────────────────────────────────────────────────────────────

struct io_write_arguments {
	struct IO_Event_Selector_IOCP         *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
};

static VALUE
io_write_submit(VALUE _arguments)
{
	struct io_write_arguments *args = (struct io_write_arguments *)_arguments;

	IO_Event_Selector_loop_yield(&args->selector->backend);

	return RB_INT2NUM(args->waiting->result);
}

static VALUE
io_write_ensure(VALUE _arguments)
{
	struct io_write_arguments *args = (struct io_write_arguments *)_arguments;

	completion_cancel_submitted(args->selector, args->waiting);
	return Qnil;
}

static int
submit_write(struct IO_Event_Selector_IOCP *selector,
             struct IO_Event_Selector_IOCP_Waiting *waiting,
             int is_socket, HANDLE handle, SOCKET socket,
             const void *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;
	memset(&c->overlapped, 0, sizeof(c->overlapped));

	if (is_socket) {
		WSABUF wsabuf = {(ULONG)len, (char *)base};
		DWORD  bytes = 0;
		completion_submit(selector, c);
		int rc = WSASend(socket, &wsabuf, 1, &bytes, 0,
		                 &c->overlapped, NULL);
		if (rc == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	} else {
		DWORD bytes = 0;
		completion_submit(selector, c);
		if (!WriteFile(handle, base, (DWORD)len, &bytes,
		               &c->overlapped)) {
			DWORD err = GetLastError();
			if (err != ERROR_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	}
	return 0;
}

static int
do_write(struct IO_Event_Selector_IOCP *selector,
         VALUE fiber, int is_socket, HANDLE handle, SOCKET socket,
         const char *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.handle = handle,
	};
	RB_OBJ_WRITTEN(selector->backend.self, Qundef, fiber);

	completion_acquire(selector, &waiting);

	int submit_result = submit_write(selector, &waiting, is_socket, handle,
	                                 socket, base, len);
	if (submit_result < 0) {
		completion_cancel(selector, &waiting);
		return submit_result;
	}

	struct io_write_arguments args = {.selector = selector,
	                                  .waiting  = &waiting};

	VALUE rv = rb_ensure(io_write_submit, (VALUE)&args,
	                     io_write_ensure,  (VALUE)&args);
	return RB_NUM2INT(rv);
}

VALUE
IO_Event_Selector_IOCP_io_write(VALUE self, VALUE fiber, VALUE io,
                                 VALUE buffer, VALUE _length, VALUE _offset)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	int    descriptor = IO_Event_Selector_io_descriptor(io);
	HANDLE handle     = (HANDLE)rb_w32_get_osfhandle(descriptor);
	int    is_socket  = rb_w32_is_socket(descriptor);
	SOCKET socket     = (SOCKET)handle;

	ensure_associated(selector, handle);

	const void *base;
	size_t      size;
	rb_io_buffer_get_bytes_for_reading(buffer, &base, &size);

	size_t length = NUM2SIZET(_length);
	size_t offset = NUM2SIZET(_offset);
	size_t total  = 0;

	if (length > size)
		rb_raise(rb_eRuntimeError, "Length exceeds size of buffer!");
	if (offset > size)
		return rb_fiber_scheduler_io_result(-1, EINVAL);

	size_t maximum_size = size - offset;
	while (maximum_size) {
		int result = do_write(selector, fiber, is_socket, handle, socket,
		                      (const char *)base + offset, maximum_size);
		if (result > 0) {
			total  += result;
			offset += result;
			if ((size_t)result >= length) break;
			length -= result;
		} else if (result == 0) {
			break;
		} else if (length > 0 && IO_Event_try_again(-result)) {
			IO_Event_Selector_IOCP_io_wait(self, fiber, io,
			                               RB_INT2NUM(IO_EVENT_WRITABLE));
		} else {
			return rb_fiber_scheduler_io_result(-1, -result);
		}
		maximum_size = size - offset;
	}

	return rb_fiber_scheduler_io_result((int)total, 0);
}

static VALUE IO_Event_Selector_IOCP_io_write_compatible(int argc, VALUE *argv,
                                                         VALUE self)
{
	rb_check_arity(argc, 4, 5);
	VALUE offset = (argc == 5) ? argv[4] : SIZET2NUM(0);
	return IO_Event_Selector_IOCP_io_write(self, argv[0], argv[1],
	                                        argv[2], argv[3], offset);
}

#endif // HAVE_RUBY_IO_BUFFER_H

// ─── select (event loop) ─────────────────────────────────────────────────────

static DWORD
make_timeout_ms(VALUE duration)
{
	if (duration == Qnil) return INFINITE;
	if (RB_INTEGER_TYPE_P(duration)) return (DWORD)(NUM2TIMET(duration) * 1000);
	double secs = RFLOAT_VALUE(rb_to_float(duration));
	if (secs <= 0.0) return 0;
	return (DWORD)(secs * 1000.0);
}

// Process all completions currently in the queue.  Returns the number
// of completions handled.
static int
process_completions(struct IO_Event_Selector_IOCP *selector, DWORD timeout_ms)
{
	OVERLAPPED_ENTRY *entries = selector->completion_entries;
	ULONG count = 0;

	if (!GetQueuedCompletionStatusEx(selector->port, entries,
	                                  IOCP_MAX_EVENTS, &count,
	                                  timeout_ms, FALSE)) {
		DWORD err = GetLastError();
		if (err == WAIT_TIMEOUT || err == ERROR_ABANDONED_WAIT_0)
			return 0;
		// Other errors are fatal.
		rb_syserr_fail((int)rb_w32_map_errno(err),
		               "process_completions:GetQueuedCompletionStatusEx");
	}

	int handled = 0;

	for (ULONG i = 0; i < count; i++) {
		OVERLAPPED_ENTRY *e = &entries[i];

		// NULL overlapped == wakeup sentinel.
		if (!e->lpOverlapped)
			continue;

		struct IO_Event_Selector_IOCP_Completion *c =
		    (struct IO_Event_Selector_IOCP_Completion *)e->lpOverlapped;

		// Free notify data (process/WSAEvent handles) if present.
		if (c->notify) {
			struct IO_Event_Selector_IOCP_Notify *notify = c->notify;
			if (notify->process) CloseHandle(notify->process);
			notify_close_wsa_event(notify);
			free(notify);
			c->notify = NULL;
		}

		ULONG_PTR status = e->Internal;
		DWORD bytes = e->dwNumberOfBytesTransferred;

		struct IO_Event_Selector_IOCP_Waiting *waiting = c->waiting;

		if (waiting) {
			if (status == IOCP_STATUS_CANCELLED) {
				// The ensure block already called completion_detach; nothing to do.
			} else if (e->lpCompletionKey == IOCP_KEY_NOTIFY) {
				// Notify-based (process or writable): bytes carries the event.
				waiting->result = (bytes > 0) ? (int)bytes : 0;
			} else if (c->readiness) {
				waiting->result = c->readiness;
			} else {
				waiting->result = iocp_result(status, bytes);
			}

			VALUE fiber = waiting->fiber;
			completion_release(selector, c);

			if (fiber) {
				handled += 1;
				IO_Event_Selector_loop_resume(&selector->backend, fiber, 0, NULL);
			}
		} else {
			// Cancelled operation whose ensure already ran: just release slot.
			completion_release(selector, c);
		}
	}

	return handled;
}

// Unblocking function: called by Ruby from another thread when it needs
// to interrupt a thread blocked inside GetQueuedCompletionStatusEx.
// We post a wakeup sentinel to the IOCP to unblock the wait.
static void
select_ubf(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	PostQueuedCompletionStatus(selector->port, 0, 0, NULL);
}

struct select_arguments {
	struct IO_Event_Selector_IOCP *selector;
	DWORD  timeout_ms;
	int    result;
};

static void *
select_internal(void *_arguments)
{
	struct select_arguments *args = _arguments;
	args->result = process_completions(args->selector, args->timeout_ms);
	return NULL;
}

VALUE
IO_Event_Selector_IOCP_select(VALUE self, VALUE duration)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	selector->idle_duration.tv_sec  = 0;
	selector->idle_duration.tv_nsec = 0;

	DWORD timeout_ms = make_timeout_ms(duration);
	int ready = IO_Event_Selector_ready_flush(&selector->backend);
	InterlockedExchange(&selector->selecting, 1);
	InterlockedExchange(&selector->wakeup_pending, 0);

	// Non-blocking pass first (like kqueue/epoll do).
	int result = process_completions(selector, 0);

	if (!ready && !result && !selector->backend.ready) {
		if (timeout_ms != 0 &&
		    InterlockedCompareExchange(&selector->wakeup_pending, 0, 0) == 0) {
			struct select_arguments args = {
				.selector   = selector,
				.timeout_ms = timeout_ms,
				.result     = 0,
			};

			struct timespec start_time;
			IO_Event_Time_current(&start_time);

			InterlockedExchange(&selector->blocked, 1);
			rb_thread_call_without_gvl(select_internal, &args,
			                           select_ubf, selector);
			InterlockedExchange(&selector->blocked, 0);

			struct timespec end_time;
			IO_Event_Time_current(&end_time);
			IO_Event_Time_elapsed(&start_time, &end_time,
			                      &selector->idle_duration);

			result = args.result;
		}
	}

	InterlockedExchange(&selector->wakeup_pending, 0);
	InterlockedExchange(&selector->selecting, 0);

	return RB_INT2NUM(result);
}

// ─── wakeup ──────────────────────────────────────────────────────────────────

VALUE
IO_Event_Selector_IOCP_wakeup(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	LONG selecting = InterlockedCompareExchange(&selector->selecting, 1, 1);
	if (selecting) {
		InterlockedExchange(&selector->wakeup_pending, 1);
		// NULL overlapped is the wakeup sentinel recognised in process_completions.
		PostQueuedCompletionStatus(selector->port, 0, 0, NULL);
		return Qtrue;
	}

	return Qfalse;
}

// ─── Init ─────────────────────────────────────────────────────────────────────

void
Init_IO_Event_Selector_IOCP(VALUE IO_Event_Selector)
{
	VALUE klass = rb_define_class_under(IO_Event_Selector, "IOCP", rb_cObject);

	rb_define_alloc_func(klass, IO_Event_Selector_IOCP_allocate);
	rb_define_method(klass, "initialize", IO_Event_Selector_IOCP_initialize, 1);

	rb_define_method(klass, "loop",          IO_Event_Selector_IOCP_loop, 0);
	rb_define_method(klass, "idle_duration", IO_Event_Selector_IOCP_idle_duration, 0);

	rb_define_method(klass, "transfer", IO_Event_Selector_IOCP_transfer, 0);
	rb_define_method(klass, "resume",   IO_Event_Selector_IOCP_resume,  -1);
	rb_define_method(klass, "yield",    IO_Event_Selector_IOCP_yield,    0);
	rb_define_method(klass, "push",     IO_Event_Selector_IOCP_push,     1);
	rb_define_method(klass, "raise",    IO_Event_Selector_IOCP_raise,   -1);

	rb_define_method(klass, "ready?",  IO_Event_Selector_IOCP_ready_p, 0);

	rb_define_method(klass, "select",  IO_Event_Selector_IOCP_select,  1);
	rb_define_method(klass, "wakeup",  IO_Event_Selector_IOCP_wakeup,  0);
	rb_define_method(klass, "close",   IO_Event_Selector_IOCP_close,   0);

	rb_define_method(klass, "io_wait", IO_Event_Selector_IOCP_io_wait, 3);

#ifdef HAVE_RUBY_IO_BUFFER_H
	rb_define_method(klass, "io_read",  IO_Event_Selector_IOCP_io_read_compatible,  -1);
	rb_define_method(klass, "io_write", IO_Event_Selector_IOCP_io_write_compatible, -1);
#endif

	rb_define_method(klass, "process_wait",
	                 IO_Event_Selector_IOCP_process_wait, 3);
}

#endif // _WIN32
