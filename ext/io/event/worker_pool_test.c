// worker_pool_test.c - Test functions for WorkerPool cancellation
// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "worker_pool_test.h"

#include <ruby/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>

static ID id_duration;

struct BusyOperationData {
  int read_fd;
  int write_fd;
  volatile int cancelled;
  double duration;  // How long to wait (for testing)
  clock_t start_time;
  clock_t end_time;
  int operation_result;
  VALUE exception;
  
  // Reference counting for safe heap management
  _Atomic int ref_count;
};

// Reference counting functions for safe heap management
static struct BusyOperationData* busy_data_create(int read_fd, int write_fd, double duration) {
	struct BusyOperationData *data = malloc(sizeof(struct BusyOperationData));
	if (!data) return NULL;
	
	memset(data, 0, sizeof(struct BusyOperationData));
	data->read_fd = read_fd;
	data->write_fd = write_fd;
	data->duration = duration;
	data->exception = Qnil;
	atomic_store(&data->ref_count, 1);
	
	return data;
}

static struct BusyOperationData* busy_data_retain(struct BusyOperationData* data) {
	if (data) {
		atomic_fetch_add(&data->ref_count, 1);
	}
	return data;
}

static void busy_data_release(struct BusyOperationData* data) {
	if (data && atomic_fetch_sub(&data->ref_count, 1) == 1) {
		// Last reference, safe to cleanup
		close(data->read_fd);
		close(data->write_fd);
		free(data);
	}
}

// The actual blocking operation that can be cancelled
static void* busy_blocking_operation(void *data) {
	struct BusyOperationData *busy_data = (struct BusyOperationData*)data;

	// Retain reference while we're using it
	busy_data_retain(busy_data);
	
	// Use select() to wait for the pipe to become readable
	fd_set read_fds;
	struct timeval timeout;
	
	FD_ZERO(&read_fds);
	FD_SET(busy_data->read_fd, &read_fds);
	
	// Set timeout based on duration
	timeout.tv_sec = (long)busy_data->duration;
	timeout.tv_usec = ((busy_data->duration - timeout.tv_sec) * 1000000);
	
	// This will block until:
	// 1. The pipe becomes readable (cancellation)
	// 2. The timeout expires
	// 3. An error occurs
	int result = select(busy_data->read_fd + 1, &read_fds, NULL, NULL, &timeout);

	void* return_value;
	if (result > 0 && FD_ISSET(busy_data->read_fd, &read_fds)) {
		// Pipe became readable - we were cancelled
		char buffer;
		read(busy_data->read_fd, &buffer, 1);  // Consume the byte
		busy_data->cancelled = 1;
		return_value = (void*)-1;  // Indicate cancellation
	} else if (result == 0) {
		// Timeout - operation completed normally
		return_value = (void*)0;  // Indicate success
	} else {
		// Error occurred
		return_value = (void*)-2;  // Indicate error
	}
	
	// Release reference before returning
	busy_data_release(busy_data);
	return return_value;
}

// Unblock function that writes to the pipe to cancel the operation
static void busy_unblock_function(void *data) {
	struct BusyOperationData *busy_data = (struct BusyOperationData*)data;
	
	// Retain reference while we're using it
	busy_data_retain(busy_data);
	
	busy_data->cancelled = 1;

	// Write a byte to the pipe to wake up the select()
	char wake_byte = 1;
	write(busy_data->write_fd, &wake_byte, 1);

	// Release reference
	busy_data_release(busy_data);
}

// Function for the main operation execution (for rb_rescue)
static VALUE busy_operation_execute(VALUE data_value) {
	struct BusyOperationData *busy_data = (struct BusyOperationData*)data_value;
	
	// Record start time
	busy_data->start_time = clock();
	
	// Execute the blocking operation
	void *block_result = rb_nogvl(
		busy_blocking_operation,
		busy_data,
		busy_unblock_function,
		busy_data,
		RB_NOGVL_UBF_ASYNC_SAFE | RB_NOGVL_OFFLOAD_SAFE
	);
	
	// Record end time
	busy_data->end_time = clock();
	
	// Store the operation result
	busy_data->operation_result = (int)(intptr_t)block_result;
	
	return Qnil;
}

// Function for exception handling (for rb_rescue)
static VALUE busy_operation_rescue(VALUE data_value, VALUE exception) {
	struct BusyOperationData *busy_data = (struct BusyOperationData*)data_value;
	
	// Record end time even in case of exception
	busy_data->end_time = clock();
	
	// Mark that an exception was caught
	busy_data->exception = exception;
	
	return exception;
}

// Ruby method: IO::Event::WorkerPool.busy(duration: 1.0)
// This creates a cancellable blocking operation for testing
static VALUE worker_pool_test_busy(int argc, VALUE *argv, VALUE self) {
	double duration = 1.0;  // Default 1 second
	
	// Extract keyword arguments
	VALUE kwargs = Qnil;
	VALUE rb_duration = Qnil;
	
	rb_scan_args(argc, argv, "0:", &kwargs);
	
	if (!NIL_P(kwargs)) {
		VALUE kwvals[1];
		ID kwkeys[1] = {id_duration};
		rb_get_kwargs(kwargs, kwkeys, 0, 1, kwvals);
		rb_duration = kwvals[0];
	}
	
	if (!NIL_P(rb_duration)) {
		duration = NUM2DBL(rb_duration);
	}
	
	// Create pipe for cancellation
	int pipe_fds[2];
	if (pipe(pipe_fds) != 0) {
		rb_sys_fail("pipe creation failed");
	}
	
	// Heap allocate operation data with reference counting
	struct BusyOperationData *busy_data = busy_data_create(pipe_fds[0], pipe_fds[1], duration);
	if (!busy_data) {
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		rb_raise(rb_eNoMemError, "failed to allocate busy operation data");
	}
	
	// Execute the blocking operation with exception handling using function pointers
	rb_rescue(
		busy_operation_execute,
		(VALUE)busy_data,
		busy_operation_rescue,
		(VALUE)busy_data
	);
	
	// Calculate elapsed time from the state stored in busy_data
	double elapsed = ((double)(busy_data->end_time - busy_data->start_time)) / CLOCKS_PER_SEC;
	
	// Create result hash using the state from busy_data
	VALUE result = rb_hash_new();
	rb_hash_aset(result, ID2SYM(rb_intern("duration")), DBL2NUM(duration));
	rb_hash_aset(result, ID2SYM(rb_intern("elapsed")), DBL2NUM(elapsed));
	
	// Determine result based on operation outcome
	if (busy_data->exception != Qnil) {
		rb_hash_aset(result, ID2SYM(rb_intern("result")), ID2SYM(rb_intern("exception")));
		rb_hash_aset(result, ID2SYM(rb_intern("cancelled")), Qtrue);
		rb_hash_aset(result, ID2SYM(rb_intern("exception")), busy_data->exception);
	} else if (busy_data->operation_result == -1) {
		rb_hash_aset(result, ID2SYM(rb_intern("result")), ID2SYM(rb_intern("cancelled")));
		rb_hash_aset(result, ID2SYM(rb_intern("cancelled")), Qtrue);
	} else if (busy_data->operation_result == 0) {
		rb_hash_aset(result, ID2SYM(rb_intern("result")), ID2SYM(rb_intern("completed")));
		rb_hash_aset(result, ID2SYM(rb_intern("cancelled")), Qfalse);
	} else {
		rb_hash_aset(result, ID2SYM(rb_intern("result")), ID2SYM(rb_intern("error")));
		rb_hash_aset(result, ID2SYM(rb_intern("cancelled")), Qfalse);
	}
	
	// Release our reference to the busy_data
	// The blocking operation and unblock function may still have references
	busy_data_release(busy_data);
	
	return result;
}

// Initialize the test functions
void Init_IO_Event_WorkerPool_Test(VALUE IO_Event_WorkerPool) {
	// Initialize symbols
	id_duration = rb_intern("duration");
	
	// Add test methods to IO::Event::WorkerPool class
	rb_define_singleton_method(IO_Event_WorkerPool, "busy", worker_pool_test_busy, -1);
} 
