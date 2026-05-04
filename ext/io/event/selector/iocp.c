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

// Pool entry.  OVERLAPPED *must* be the first field so that we can cast
// between (LPOVERLAPPED) and (struct IO_Event_Selector_IOCP_Completion *).
// This is the pointer that travels through the IOCP queue; the stack-allocated
// Waiting struct is never referenced from the queue, giving safe cancellation.
struct IO_Event_Selector_IOCP_Completion {
	OVERLAPPED overlapped;    // ← must be first
	struct IO_Event_List list;

	// Back-pointer to the stack-allocated Waiting.  Nulled by waiting_cancel()
	// before the stack frame is released, so process_completions never chases
	// a dangling pointer.
	struct IO_Event_Selector_IOCP_Waiting *waiting;

	// Opaque auxiliary data (e.g. process / WSAEvent handles for notify-based
	// operations).  Always freed by process_completions, never by ensure.
	void *aux;
};

// Stack-allocated per-operation state.  Lives in the suspended fiber's stack
// for the duration of the operation.
struct IO_Event_Selector_IOCP_Waiting {
	struct IO_Event_Selector_IOCP_Completion *completion;

	VALUE  fiber;
	int    result;   // bytes transferred (≥0), or negated errno (<0)
	HANDLE handle;   // file/socket handle, for CancelIoEx
};

// Per-operation auxiliary state for RegisterWaitForSingleObject-based waits
// (process_wait and io_wait-writable).  Heap-allocated; always freed in
// process_completions so it outlives any cancellation in ensure.
struct IO_Event_Selector_IOCP_Notify {
	HANDLE           port;        // back-pointer to the IOCP
	OVERLAPPED      *overlapped;  // points to completion->overlapped
	volatile LONG    posted;      // 1 once PostQueuedCompletionStatus is called
	HANDLE           process;     // non-NULL for process_wait
	WSAEVENT         wsa_event;   // non-NULL for io_wait-writable
	HANDLE           wait_handle; // from RegisterWaitForSingleObject
};

struct IO_Event_Selector_IOCP {
	struct IO_Event_Selector backend;

	HANDLE port;      // I/O Completion Port
	int    blocked;   // 1 while sleeping in GetQueuedCompletionStatusEx

	struct timespec idle_duration;

	struct IO_Event_Array completions; // pool of IO_Event_Selector_IOCP_Completion
	struct IO_Event_List  free_list;
};

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

static void
close_internal(struct IO_Event_Selector_IOCP *selector)
{
	if (selector->port && selector->port != INVALID_HANDLE_VALUE) {
		CloseHandle(selector->port);
		selector->port = INVALID_HANDLE_VALUE;
	}
}

static void
IO_Event_Selector_IOCP_Type_free(void *_selector)
{
	struct IO_Event_Selector_IOCP *selector = _selector;
	close_internal(selector);
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
	c->aux     = NULL;
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
		c = (struct IO_Event_Selector_IOCP_Completion *)selector->free_list.tail;
		IO_Event_List_pop(&c->list);
	} else {
		c = IO_Event_Array_push(&selector->completions);
		IO_Event_List_clear(&c->list);
	}

	memset(&c->overlapped, 0, sizeof(c->overlapped));
	c->waiting = waiting;
	c->aux     = NULL;
	waiting->completion = c;

	return c;
}

static void
completion_release(struct IO_Event_Selector_IOCP *selector,
                   struct IO_Event_Selector_IOCP_Completion *c)
{
	c->waiting = NULL;
	c->aux     = NULL;
	IO_Event_List_prepend(&selector->free_list, &c->list);
}

static void
waiting_cancel(struct IO_Event_Selector_IOCP_Waiting *waiting)
{
	if (waiting->completion) {
		waiting->completion->waiting = NULL;
		waiting->completion = NULL;
	}
	waiting->fiber = 0;
}

// ─── Handle helpers ───────────────────────────────────────────────────────────

// Lazily associate a handle with our IOCP.  Safe to call multiple times on the
// same handle: if it is already associated with *this* port the call succeeds
// and is a no-op; if it belongs to another port we get ERROR_INVALID_PARAMETER
// which we surface as a Ruby exception.
static void
ensure_associated(struct IO_Event_Selector_IOCP *selector, HANDLE h)
{
	HANDLE result = CreateIoCompletionPort(h, selector->port, IOCP_KEY_IO, 0);
	if (result == NULL) {
		DWORD err = GetLastError();
		// ERROR_INVALID_PARAMETER can mean "already associated with this port"
		// on some Windows versions — treat it as success.
		if (err != ERROR_INVALID_PARAMETER)
			rb_syserr_fail((int)rb_w32_map_errno(err),
			               "ensure_associated:CreateIoCompletionPort");
	}
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
	selector->blocked = 0;

	IO_Event_List_initialize(&selector->free_list);

	selector->completions.element_initialize =
	    IO_Event_Selector_IOCP_Completion_initialize;
	selector->completions.element_free =
	    IO_Event_Selector_IOCP_Completion_free_element;

	if (IO_Event_Array_initialize(&selector->completions,
	                               IO_EVENT_ARRAY_DEFAULT_COUNT,
	                               sizeof(struct IO_Event_Selector_IOCP_Completion)) < 0)
		rb_sys_fail("IO_Event_Selector_IOCP_allocate:IO_Event_Array_initialize");

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
	close_internal(selector);
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
	struct IO_Event_Selector_IOCP_Completion *c = args->waiting->completion;

	if (c) {
		struct IO_Event_Selector_IOCP_Notify *notify = c->aux;

		// Disconnect the stack waiting so process_completions won't try to
		// resume a dead fiber.
		c->waiting = NULL;

		// Ensure exactly one completion arrives so process_completions can
		// free the notify struct and release the completion slot.
		if (notify && InterlockedCompareExchange(&notify->posted, 1, 0) == 0) {
			PostQueuedCompletionStatus(args->selector->port, 0,
			                           IOCP_KEY_NOTIFY, &c->overlapped);
		}

		// Non-blocking unregister — the callback may have already fired.
		if (notify && notify->wait_handle)
			UnregisterWait(notify->wait_handle);
	}

	waiting_cancel(args->waiting);
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
		rb_memerror();
	}
	notify->port        = selector->port;
	notify->overlapped  = &c->overlapped;
	notify->posted      = 0;
	notify->process     = process;
	notify->wsa_event   = NULL;
	notify->wait_handle = NULL;
	c->aux = notify;

	if (!RegisterWaitForSingleObject(&notify->wait_handle, process,
	                                 process_exit_callback, notify,
	                                 INFINITE, WT_EXECUTEONLYONCE)) {
		free(notify);
		c->aux = NULL;
		CloseHandle(process);
		waiting_cancel(&waiting);
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
	int requested_events;
};

static VALUE
io_wait_ensure(VALUE _arguments)
{
	struct io_wait_arguments *args =
	    (struct io_wait_arguments *)_arguments;
	struct IO_Event_Selector_IOCP_Completion *c = args->waiting->completion;

	if (c) {
		// Cancel any in-flight overlapped read (readable wait) or
		// force-flush a notify-based write wait.
		if (c->aux) {
			// Notify-based (io_wait writable): same pattern as process_wait.
			struct IO_Event_Selector_IOCP_Notify *notify = c->aux;
			c->waiting = NULL;
			if (InterlockedCompareExchange(&notify->posted, 1, 0) == 0)
				PostQueuedCompletionStatus(args->selector->port, 0,
				                           IOCP_KEY_NOTIFY, &c->overlapped);
			if (notify->wait_handle)
				UnregisterWait(notify->wait_handle);
		} else {
			// Overlapped-IO based (io_wait readable): cancel via CancelIoEx.
			c->waiting = NULL;
			CancelIoEx(args->waiting->handle, &c->overlapped);
		}
	}

	waiting_cancel(args->waiting);
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

	if (notify->wsa_event) {
		WSACloseEvent(notify->wsa_event);
		notify->wsa_event = NULL;
	}

	if (InterlockedCompareExchange(&notify->posted, 1, 0) == 0) {
		PostQueuedCompletionStatus(notify->port, IO_EVENT_WRITABLE,
		                           IOCP_KEY_NOTIFY, notify->overlapped);
	}
}

VALUE
IO_Event_Selector_IOCP_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	int fd              = IO_Event_Selector_io_descriptor(io);
	int requested       = RB_NUM2INT(events);
	SOCKET sock         = rb_w32_get_osfhandle(fd);
	int    is_socket    = rb_w32_is_socket(fd);

	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.handle = (HANDLE)sock,
	};
	RB_OBJ_WRITTEN(self, Qundef, fiber);

	struct IO_Event_Selector_IOCP_Completion *c =
	    completion_acquire(selector, &waiting);

	struct io_wait_arguments args = {
		.selector         = selector,
		.waiting          = &waiting,
		.requested_events = requested,
	};

	if (requested & IO_EVENT_READABLE) {
		// Zero-byte overlapped read: completes when data is available.
		// Works for both sockets (WSARecv) and overlapped-capable files.
		if (is_socket) {
			ensure_associated(selector, (HANDLE)sock);
			WSABUF wsabuf = {0, NULL};
			DWORD  bytes = 0, wsa_flags = 0;
			int rc = WSARecv(sock, &wsabuf, 1, &bytes, &wsa_flags,
			                 &c->overlapped, NULL);
			if (rc == SOCKET_ERROR) {
				int err = WSAGetLastError();
				if (err != WSA_IO_PENDING) {
					waiting_cancel(&waiting);
					rb_syserr_fail(rb_w32_map_errno(err),
					               "io_wait:WSARecv");
				}
			}
		} else {
			ensure_associated(selector, (HANDLE)sock);
			// Zero-byte ReadFile: pends until data is available on a pipe.
			DWORD bytes = 0;
			if (!ReadFile((HANDLE)sock, NULL, 0, &bytes, &c->overlapped)) {
				DWORD err = GetLastError();
				if (err != ERROR_IO_PENDING) {
					waiting_cancel(&waiting);
					rb_syserr_fail(rb_w32_map_errno(err),
					               "io_wait:ReadFile");
				}
			}
		}

		// Set result to indicate what we're waiting for; overwritten by
		// process_completions on actual completion.
		waiting.result = IO_EVENT_READABLE;

	} else if (requested & IO_EVENT_WRITABLE) {
		if (is_socket) {
			// Use WSAEventSelect + OS thread pool for writable detection.
			// FD_WRITE fires immediately for freshly-connected sockets and
			// after WSAEWOULDBLOCK on send; FD_CONNECT fires on async connect.
			// We avoid select()+FD_SET here because ruby/win32.h redefines
			// FD_SET to call _get_osfhandle(fd) expecting a CRT integer fd,
			// not a raw SOCKET handle, which would produce wrong results.
			WSAEVENT wsa_ev = WSACreateEvent();
			if (wsa_ev == WSA_INVALID_EVENT) {
				waiting_cancel(&waiting);
				completion_release(selector, c);
				rb_syserr_fail(rb_w32_map_errno(WSAGetLastError()),
				               "io_wait:WSACreateEvent");
			}

			// FD_WRITE fires when previously-blocked send becomes possible;
			// FD_CONNECT fires when an async connect completes.
			WSAEventSelect(sock, wsa_ev, FD_WRITE | FD_CONNECT);

			struct IO_Event_Selector_IOCP_Notify *notify =
			    malloc(sizeof(*notify));
			if (!notify) {
				WSACloseEvent(wsa_ev);
				waiting_cancel(&waiting);
				completion_release(selector, c);
				rb_memerror();
			}
			notify->port        = selector->port;
			notify->overlapped  = &c->overlapped;
			notify->posted      = 0;
			notify->process     = NULL;
			notify->wsa_event   = wsa_ev;
			notify->wait_handle = NULL;
			c->aux = notify;

			if (!RegisterWaitForSingleObject(&notify->wait_handle, wsa_ev,
			                                 io_wait_writable_callback,
			                                 notify, INFINITE,
			                                 WT_EXECUTEONLYONCE)) {
				WSACloseEvent(wsa_ev);
				free(notify);
				c->aux = NULL;
				waiting_cancel(&waiting);
				completion_release(selector, c);
				rb_sys_fail("io_wait:RegisterWaitForSingleObject");
			}

			waiting.result = IO_EVENT_WRITABLE;
		} else {
			// Pipes opened with FILE_FLAG_OVERLAPPED are almost always
			// writable; enqueue as ready and let io_write handle backpressure.
			waiting.result = IO_EVENT_WRITABLE;
			IO_Event_Selector_ready_push(&selector->backend, fiber);
			waiting_cancel(&waiting);
			completion_release(selector, c);
			IO_Event_Selector_loop_yield(&selector->backend);
			return RB_INT2NUM(IO_EVENT_WRITABLE);
		}
	}

	return rb_ensure(io_wait_transfer, (VALUE)&args,
	                 io_wait_ensure,   (VALUE)&args);
}

// ─── io_read / io_write ───────────────────────────────────────────────────────

#ifdef HAVE_RUBY_IO_BUFFER_H

struct io_read_arguments {
	struct IO_Event_Selector_IOCP         *selector;
	struct IO_Event_Selector_IOCP_Waiting *waiting;
	VALUE self;
	VALUE fiber;
	VALUE io;
	int   descriptor;
	int   is_socket;
};

static int
submit_read(struct IO_Event_Selector_IOCP *selector,
            struct IO_Event_Selector_IOCP_Waiting *waiting,
            int is_socket, SOCKET sock,
            void *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;

	memset(&c->overlapped, 0, sizeof(c->overlapped));

	if (is_socket) {
		WSABUF wsabuf = {(ULONG)len, (char *)base};
		DWORD  bytes = 0, flags = 0;
		int rc = WSARecv(sock, &wsabuf, 1, &bytes, &flags,
		                 &c->overlapped, NULL);
		if (rc == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	} else {
		DWORD bytes = 0;
		if (!ReadFile((HANDLE)sock, base, (DWORD)len, &bytes,
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
	return RB_INT2NUM(args->waiting->result);
}

static VALUE
io_read_ensure(VALUE _arguments)
{
	struct io_read_arguments *args = (struct io_read_arguments *)_arguments;
	struct IO_Event_Selector_IOCP_Completion *c = args->waiting->completion;
	if (c) {
		c->waiting = NULL;
		CancelIoEx(args->waiting->handle, &c->overlapped);
	}
	waiting_cancel(args->waiting);
	return Qnil;
}

static int
do_read(struct IO_Event_Selector_IOCP *selector,
        VALUE self, VALUE fiber, VALUE io,
        int is_socket, SOCKET sock,
        char *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.handle = (HANDLE)sock,
	};
	RB_OBJ_WRITTEN(selector->backend.self, Qundef, fiber);

	completion_acquire(selector, &waiting);

	int submit_result = submit_read(selector, &waiting, is_socket, sock,
	                                base, len);
	if (submit_result < 0) {
		waiting_cancel(&waiting);
		return submit_result;
	}

	struct io_read_arguments args = {
		.selector   = selector,
		.waiting    = &waiting,
		.self       = self,
		.fiber      = fiber,
		.io         = io,
		.descriptor = (int)(SOCKET)sock,
		.is_socket  = is_socket,
	};

	IO_Event_Selector_loop_yield(&selector->backend);

	// ensure always runs; call it manually here via rb_ensure pattern.
	// (We use rb_ensure so exceptions are handled properly.)
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

	int    fd        = IO_Event_Selector_io_descriptor(io);
	SOCKET sock      = rb_w32_get_osfhandle(fd);
	int    is_socket = rb_w32_is_socket(fd);

	ensure_associated(selector, (HANDLE)sock);

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
		                     is_socket, sock,
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
	return RB_INT2NUM(args->waiting->result);
}

static VALUE
io_write_ensure(VALUE _arguments)
{
	struct io_write_arguments *args = (struct io_write_arguments *)_arguments;
	struct IO_Event_Selector_IOCP_Completion *c = args->waiting->completion;
	if (c) {
		c->waiting = NULL;
		CancelIoEx(args->waiting->handle, &c->overlapped);
	}
	waiting_cancel(args->waiting);
	return Qnil;
}

static int
submit_write(struct IO_Event_Selector_IOCP *selector,
             struct IO_Event_Selector_IOCP_Waiting *waiting,
             int is_socket, SOCKET sock,
             const void *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Completion *c = waiting->completion;
	memset(&c->overlapped, 0, sizeof(c->overlapped));

	if (is_socket) {
		WSABUF wsabuf = {(ULONG)len, (char *)base};
		DWORD  bytes = 0;
		int rc = WSASend(sock, &wsabuf, 1, &bytes, 0,
		                 &c->overlapped, NULL);
		if (rc == SOCKET_ERROR) {
			int err = WSAGetLastError();
			if (err != WSA_IO_PENDING)
				return -(int)rb_w32_map_errno(err);
		}
	} else {
		DWORD bytes = 0;
		if (!WriteFile((HANDLE)sock, base, (DWORD)len, &bytes,
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
         VALUE fiber, int is_socket, SOCKET sock,
         const char *base, size_t len)
{
	struct IO_Event_Selector_IOCP_Waiting waiting = {
		.fiber  = fiber,
		.handle = (HANDLE)sock,
	};
	RB_OBJ_WRITTEN(selector->backend.self, Qundef, fiber);

	completion_acquire(selector, &waiting);

	int submit_result = submit_write(selector, &waiting, is_socket, sock,
	                                 base, len);
	if (submit_result < 0) {
		waiting_cancel(&waiting);
		return submit_result;
	}

	struct io_write_arguments args = {.selector = selector,
	                                  .waiting  = &waiting};

	IO_Event_Selector_loop_yield(&selector->backend);

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

	int    fd        = IO_Event_Selector_io_descriptor(io);
	SOCKET sock      = rb_w32_get_osfhandle(fd);
	int    is_socket = rb_w32_is_socket(fd);

	ensure_associated(selector, (HANDLE)sock);

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
		int result = do_write(selector, fiber, is_socket, sock,
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
	OVERLAPPED_ENTRY entries[IOCP_MAX_EVENTS];
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

	for (ULONG i = 0; i < count; i++) {
		OVERLAPPED_ENTRY *e = &entries[i];

		// NULL overlapped == wakeup sentinel.
		if (!e->lpOverlapped)
			continue;

		struct IO_Event_Selector_IOCP_Completion *c =
		    (struct IO_Event_Selector_IOCP_Completion *)e->lpOverlapped;

		// Free aux data (process/WSAEvent handles) if present.
		if (c->aux) {
			struct IO_Event_Selector_IOCP_Notify *notify = c->aux;
			if (notify->process)    CloseHandle(notify->process);
			if (notify->wsa_event)  WSACloseEvent(notify->wsa_event);
			free(notify);
			c->aux = NULL;
		}

		ULONG_PTR status = e->Internal;
		DWORD bytes = e->dwNumberOfBytesTransferred;

		struct IO_Event_Selector_IOCP_Waiting *waiting = c->waiting;

		if (waiting) {
			if (status == IOCP_STATUS_CANCELLED) {
				// The ensure block already called waiting_cancel; nothing to do.
			} else if (e->lpCompletionKey == IOCP_KEY_NOTIFY) {
				// Notify-based (process or writable): bytes carries the event.
				waiting->result = (bytes > 0) ? (int)bytes : 0;
			} else {
				waiting->result = iocp_result(status, bytes);
			}

			VALUE fiber = waiting->fiber;
			completion_release(selector, c);

			if (fiber)
				IO_Event_Selector_loop_resume(&selector->backend, fiber, 0, NULL);
		} else {
			// Cancelled operation whose ensure already ran: just release slot.
			completion_release(selector, c);
		}
	}

	return (int)count;
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

	int ready = IO_Event_Selector_ready_flush(&selector->backend);

	// Non-blocking pass first (like kqueue/epoll do).
	int result = process_completions(selector, 0);

	if (!ready && !result && !selector->backend.ready) {
		DWORD timeout_ms = make_timeout_ms(duration);
		if (timeout_ms != 0) {
			struct select_arguments args = {
				.selector   = selector,
				.timeout_ms = timeout_ms,
				.result     = 0,
			};

			struct timespec start_time;
			IO_Event_Time_current(&start_time);

			selector->blocked = 1;
			rb_thread_call_without_gvl(select_internal, &args,
			                           select_ubf, selector);
			selector->blocked = 0;

			struct timespec end_time;
			IO_Event_Time_current(&end_time);
			IO_Event_Time_elapsed(&start_time, &end_time,
			                      &selector->idle_duration);

			result = args.result;
		}
	}

	return RB_INT2NUM(result);
}

// ─── wakeup ──────────────────────────────────────────────────────────────────

VALUE
IO_Event_Selector_IOCP_wakeup(VALUE self)
{
	struct IO_Event_Selector_IOCP *selector = NULL;
	TypedData_Get_Struct(self, struct IO_Event_Selector_IOCP,
	                     &IO_Event_Selector_IOCP_Type, selector);

	if (selector->blocked) {
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
