// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "worker_pool.h"
#include "fiber.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

// Forward declarations
static VALUE IO_Event_WorkerPool;

// Thread pool structure
struct IO_Event_WorkerPool_Worker {
	pthread_t thread;
	struct IO_Event_WorkerPool *pool;
	struct IO_Event_WorkerPool_Worker *next;
};

// Work item structure
struct IO_Event_WorkerPool_Work {
	rb_fiber_scheduler_blocking_operation_t *blocking_operation;
	bool completed;
	
	struct IO_Event_WorkerPool_Work *next;
};

// Worker pool structure
struct IO_Event_WorkerPool {
	pthread_mutex_t mutex;
	pthread_cond_t work_available;
	pthread_cond_t work_completed;
	
	struct IO_Event_WorkerPool_Work *work_queue;
	struct IO_Event_WorkerPool_Work *work_queue_tail;
	
	struct IO_Event_WorkerPool_Worker *workers;
	size_t current_worker_count;
	size_t maximum_worker_count;
	
	size_t call_count;
	size_t completed_count;
	size_t cancelled_count;
	
	bool shutdown;
};

// Free functions for Ruby GC
static void worker_pool_free(void *ptr) {
	struct IO_Event_WorkerPool *pool = (struct IO_Event_WorkerPool *)ptr;
	
	if (pool) {
		// Signal shutdown
		pthread_mutex_lock(&pool->mutex);
		pool->shutdown = true;
		pthread_cond_broadcast(&pool->work_available);
		pthread_mutex_unlock(&pool->mutex);
		
		// Wait for all workers to finish
		struct IO_Event_WorkerPool_Worker *thread = pool->workers;
		while (thread) {
			pthread_join(thread->thread, NULL);
			struct IO_Event_WorkerPool_Worker *next = thread->next;
			free(thread);
			thread = next;
		}
		
		// Clean up work queue
		struct IO_Event_WorkerPool_Work *work = pool->work_queue;
		while (work) {
			struct IO_Event_WorkerPool_Work *next = work->next;
			free(work);
			work = next;
		}
		
		pthread_mutex_destroy(&pool->mutex);
		pthread_cond_destroy(&pool->work_available);
		pthread_cond_destroy(&pool->work_completed);
		
		free(pool);
	}
}

// Size functions for Ruby GC
static size_t worker_pool_size(const void *ptr) {
	return sizeof(struct IO_Event_WorkerPool);
}

// Ruby TypedData structures
static const rb_data_type_t IO_Event_WorkerPool_type = {
	"IO::Event::WorkerPool",
	{0, worker_pool_free, worker_pool_size,},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

// Worker thread function
static void* worker_thread_func(void *arg) {
	struct IO_Event_WorkerPool_Worker *worker = (struct IO_Event_WorkerPool_Worker *)arg;
	struct IO_Event_WorkerPool *pool = worker->pool;
	
	while (true) {
		struct IO_Event_WorkerPool_Work *work = NULL;
		
		pthread_mutex_lock(&pool->mutex);
		
		// Wait for work or shutdown
		while (!pool->work_queue && !pool->shutdown) {
			pthread_cond_wait(&pool->work_available, &pool->mutex);
		}
		
		if (pool->shutdown) {
			pthread_mutex_unlock(&pool->mutex);
			break;
		}
		
		// Dequeue work item
		if (pool->work_queue) {
			work = pool->work_queue;
			pool->work_queue = work->next;
			if (!pool->work_queue) {
				pool->work_queue_tail = NULL;
			}
		}
		
		pthread_mutex_unlock(&pool->mutex);
		
		// Execute work
		if (work) {
			rb_fiber_scheduler_blocking_operation_execute(work->blocking_operation);
			
			// Mark work as completed (mutex required for worker thread)
			pthread_mutex_lock(&pool->mutex);
			work->completed = true;
			pool->completed_count++;
			pthread_cond_signal(&pool->work_completed);
			pthread_mutex_unlock(&pool->mutex);
		}
	}
	
	return NULL;
}

// Create a new worker thread
static int create_worker_thread(struct IO_Event_WorkerPool *pool) {
	if (pool->current_worker_count >= pool->maximum_worker_count) {
		return -1;
	}
	
		struct IO_Event_WorkerPool_Worker *worker = malloc(sizeof(struct IO_Event_WorkerPool_Worker));
	if (!worker) {
		return -1;
	}
	
	worker->pool = pool;
	worker->next = pool->workers;
	
	if (pthread_create(&worker->thread, NULL, worker_thread_func, worker) != 0) {
		free(worker);
		return -1;
	}
	
	pool->workers = worker;
	pool->current_worker_count++;
	
	return 0;
}

// Ruby constructor for WorkerPool
static VALUE worker_pool_initialize(int argc, VALUE *argv, VALUE self) {
	VALUE rb_maximum_worker_count = Qnil;
	size_t maximum_worker_count = 4; // Default
	
	// Handle keyword arguments
	if (argc == 1 && RB_TYPE_P(argv[0], T_HASH)) {
		VALUE hash = argv[0];
		VALUE max_threads_key = ID2SYM(rb_intern("max_threads"));
		if (rb_hash_lookup(hash, max_threads_key) != Qnil) {
			rb_maximum_worker_count = rb_hash_aref(hash, max_threads_key);
		}
	} else if (argc == 1) {
		rb_maximum_worker_count = argv[0];
	} else if (argc > 1) {
		rb_raise(rb_eArgError, "wrong number of arguments (given %d, expected 0..1)!", argc);
	}
	
	if (!NIL_P(rb_maximum_worker_count)) {
		maximum_worker_count = NUM2SIZET(rb_maximum_worker_count);
		if (maximum_worker_count == 0) {
			rb_raise(rb_eArgError, "max_threads must be greater than 0!");
		}
	}
	
	// Get the pool that was allocated by worker_pool_allocate
	struct IO_Event_WorkerPool *pool;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	if (!pool) {
		rb_raise(rb_eRuntimeError, "WorkerPool allocation failed!");
	}
	
	pthread_mutex_init(&pool->mutex, NULL);
	pthread_cond_init(&pool->work_available, NULL);
	pthread_cond_init(&pool->work_completed, NULL);
	
	pool->work_queue = NULL;
	pool->work_queue_tail = NULL;
	pool->workers = NULL;
	pool->current_worker_count = 0;
	pool->maximum_worker_count = maximum_worker_count;
	pool->call_count = 0;
	pool->completed_count = 0;
	pool->cancelled_count = 0;
	pool->shutdown = false;
	
	// Create initial workers
	for (size_t i = 0; i < maximum_worker_count; i++) {
		if (create_worker_thread(pool) != 0) {
			// Just set the maximum_worker_count for debugging, don't fail completely
			// worker_pool_free(pool);
			// rb_raise(rb_eRuntimeError, "Failed to create workers");
			break;
		}
	}
	
	return self;
}

// Structure to pass both work and pool to rb_ensure functions
struct worker_pool_call_arguments {
	struct IO_Event_WorkerPool_Work *work;
	struct IO_Event_WorkerPool *pool;
};

// Cleanup function for rb_ensure
static VALUE worker_pool_call_cleanup(VALUE _arguments) {
	struct worker_pool_call_arguments *arguments = (struct worker_pool_call_arguments *)_arguments;
	if (arguments && arguments->work) {
		// Cancel the blocking operation if possible
		if (arguments->work->blocking_operation) {
			rb_fiber_scheduler_blocking_operation_cancel(arguments->work->blocking_operation);
			
			// Increment cancelled count (protected by GVL)
			arguments->pool->cancelled_count++;
		}
		free(arguments->work);
	}
	return Qnil;
}

// Main work execution function
static VALUE worker_pool_call_body(VALUE _arguments) {
	struct worker_pool_call_arguments *arguments = (struct worker_pool_call_arguments *)_arguments;
	struct IO_Event_WorkerPool_Work *work = arguments->work;
	struct IO_Event_WorkerPool *pool = arguments->pool;
	
	// Wait for completion
	pthread_mutex_lock(&pool->mutex);
	while (!work->completed) {
		pthread_cond_wait(&pool->work_completed, &pool->mutex);
	}
	pthread_mutex_unlock(&pool->mutex);
	
	return Qnil;
}

// Ruby method to submit work and wait for completion
static VALUE worker_pool_call(VALUE self, VALUE _blocking_operation) {
	struct IO_Event_WorkerPool *pool;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	if (pool->shutdown) {
		rb_raise(rb_eRuntimeError, "Worker pool is shut down!");
	}
	
	// Increment call count (protected by GVL)
	pool->call_count++;
	
	// Extract blocking operation handle
	rb_fiber_scheduler_blocking_operation_t *blocking_operation = rb_fiber_scheduler_blocking_operation_extract(_blocking_operation);
	
	if (!blocking_operation) {
		rb_raise(rb_eArgError, "Invalid blocking operation!");
	}
	
	// Create work item
	struct IO_Event_WorkerPool_Work *work = malloc(sizeof(struct IO_Event_WorkerPool_Work));
	if (!work) {
		rb_raise(rb_eNoMemError, "Failed to allocate work item!");
	}
	
	work->blocking_operation = blocking_operation;
	work->completed = false;
	work->next = NULL;
	
	// Enqueue work
	pthread_mutex_lock(&pool->mutex);
	
	if (pool->work_queue_tail) {
		pool->work_queue_tail->next = work;
	} else {
		pool->work_queue = work;
	}
	pool->work_queue_tail = work;
	
	pthread_cond_signal(&pool->work_available);
	pthread_mutex_unlock(&pool->mutex);
	
	// Wait for completion with proper cleanup using rb_ensure
	struct worker_pool_call_arguments arguments = {work, pool};
	rb_ensure(worker_pool_call_body, (VALUE)&arguments, worker_pool_call_cleanup, (VALUE)&arguments);
	
	return Qnil;
}

static VALUE worker_pool_allocate(VALUE klass) {
	struct IO_Event_WorkerPool *pool;
	VALUE self = TypedData_Make_Struct(klass, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	// Initialize to NULL/zero so we can detect uninitialized pools
	memset(pool, 0, sizeof(struct IO_Event_WorkerPool));
	
	return self;
}

// Test helper: get pool statistics for debugging/testing
static VALUE worker_pool_statistics(VALUE self) {
	struct IO_Event_WorkerPool *pool;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	if (!pool) {
		rb_raise(rb_eRuntimeError, "WorkerPool not initialized!");
	}
	
	VALUE stats = rb_hash_new();
	rb_hash_aset(stats, ID2SYM(rb_intern("current_worker_count")), SIZET2NUM(pool->current_worker_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("maximum_worker_count")), SIZET2NUM(pool->maximum_worker_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("call_count")), SIZET2NUM(pool->call_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("completed_count")), SIZET2NUM(pool->completed_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("cancelled_count")), SIZET2NUM(pool->cancelled_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("shutdown")), pool->shutdown ? Qtrue : Qfalse);
	
	// Count work items in queue (only if properly initialized)
	if (pool->maximum_worker_count > 0) {
		pthread_mutex_lock(&pool->mutex);
		size_t current_queue_size = 0;
		struct IO_Event_WorkerPool_Work *work = pool->work_queue;
		while (work) {
			current_queue_size++;
			work = work->next;
		}
		pthread_mutex_unlock(&pool->mutex);
		rb_hash_aset(stats, ID2SYM(rb_intern("current_queue_size")), SIZET2NUM(current_queue_size));
	} else {
		rb_hash_aset(stats, ID2SYM(rb_intern("current_queue_size")), SIZET2NUM(0));
	}
	
	return stats;
}

void Init_IO_Event_WorkerPool(VALUE IO_Event) {
	IO_Event_WorkerPool = rb_define_class_under(IO_Event, "WorkerPool", rb_cObject);
	rb_define_alloc_func(IO_Event_WorkerPool, worker_pool_allocate);
	rb_define_method(IO_Event_WorkerPool, "initialize", worker_pool_initialize, -1);
	rb_define_method(IO_Event_WorkerPool, "call", worker_pool_call, 1);
	rb_define_method(IO_Event_WorkerPool, "statistics", worker_pool_statistics, 0);
}
