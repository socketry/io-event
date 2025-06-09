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
static VALUE IO_Event_WorkerPool_Promise;

// Thread pool structure
typedef struct worker_thread {
	pthread_t thread;
	struct IO_Event_WorkerPool *pool;
	struct worker_thread *next;
} worker_thread_t;

// Work item structure
typedef struct work_item {
	rb_fiber_scheduler_blocking_operation_t *blocking_operation;
	VALUE promise;
	bool cancelled;
	struct work_item *next;
} work_item_t;

// Worker pool structure
struct IO_Event_WorkerPool {
	pthread_mutex_t mutex;
	pthread_cond_t work_available;
	pthread_cond_t work_completed;
	
	work_item_t *work_queue;
	work_item_t *work_queue_tail;
	
	worker_thread_t *threads;
	size_t thread_count;
	size_t max_threads;
	
	bool shutdown;
};

// Promise structure
struct IO_Event_WorkerPool_Promise {
	work_item_t *work_item;
	struct IO_Event_WorkerPool *pool;
	VALUE fiber;
	bool completed;
	bool cancelled;
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
		
		// Wait for all threads to finish
		worker_thread_t *thread = pool->threads;
		while (thread) {
			pthread_join(thread->thread, NULL);
			worker_thread_t *next = thread->next;
			free(thread);
			thread = next;
		}
		
		// Clean up work queue
		work_item_t *work = pool->work_queue;
		while (work) {
			work_item_t *next = work->next;
			free(work);
			work = next;
		}
		
		pthread_mutex_destroy(&pool->mutex);
		pthread_cond_destroy(&pool->work_available);
		pthread_cond_destroy(&pool->work_completed);
		
		free(pool);
	}
}

static void worker_pool_promise_free(void *ptr) {
	struct IO_Event_WorkerPool_Promise *promise = (struct IO_Event_WorkerPool_Promise *)ptr;
	if (promise) {
		free(promise);
	}
}

// Size functions for Ruby GC
static size_t worker_pool_size(const void *ptr) {
	return sizeof(struct IO_Event_WorkerPool);
}

static size_t worker_pool_promise_size(const void *ptr) {
	return sizeof(struct IO_Event_WorkerPool_Promise);
}

// Ruby TypedData structures
static const rb_data_type_t IO_Event_WorkerPool_type = {
	"IO::Event::WorkerPool",
	{0, worker_pool_free, worker_pool_size,},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static const rb_data_type_t IO_Event_WorkerPool_Promise_type = {
	"IO::Event::WorkerPool::Promise",
	{0, worker_pool_promise_free, worker_pool_promise_size,},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

// Worker thread function
static void* worker_thread_func(void *arg) {
	worker_thread_t *worker = (worker_thread_t *)arg;
		struct IO_Event_WorkerPool *pool = worker->pool;
	
	while (true) {
		work_item_t *work = NULL;
		
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
		
		// Execute work if not cancelled
		if (work && !work->cancelled) {
			rb_fiber_scheduler_blocking_operation_execute(work->blocking_operation);
			
			// Mark promise as completed
			struct IO_Event_WorkerPool_Promise *promise_data;
			TypedData_Get_Struct(work->promise, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise_data);
			promise_data->completed = true;
		}
		
		// Signal completion
		if (work) {
			pthread_mutex_lock(&pool->mutex);
			pthread_cond_signal(&pool->work_completed);
			pthread_mutex_unlock(&pool->mutex);
		}
	}
	
	return NULL;
}

// Create a new worker thread
static int create_worker_thread(struct IO_Event_WorkerPool *pool) {
	if (pool->thread_count >= pool->max_threads) {
		return -1;
	}
	
	worker_thread_t *worker = malloc(sizeof(worker_thread_t));
	if (!worker) {
		return -1;
	}
	
	worker->pool = pool;
	worker->next = pool->threads;
	
	if (pthread_create(&worker->thread, NULL, worker_thread_func, worker) != 0) {
		free(worker);
		return -1;
	}
	
	pool->threads = worker;
	pool->thread_count++;
	
	return 0;
}

// Ruby constructor for WorkerPool
static VALUE worker_pool_initialize(int argc, VALUE *argv, VALUE self) {
	VALUE rb_max_threads = Qnil;
	size_t max_threads = 4; // Default
	
	// Handle keyword arguments
	if (argc == 1 && RB_TYPE_P(argv[0], T_HASH)) {
		VALUE hash = argv[0];
		VALUE max_threads_key = ID2SYM(rb_intern("max_threads"));
		if (rb_hash_lookup(hash, max_threads_key) != Qnil) {
			rb_max_threads = rb_hash_aref(hash, max_threads_key);
		}
	} else if (argc == 1) {
		rb_max_threads = argv[0];
	} else if (argc > 1) {
		rb_raise(rb_eArgError, "wrong number of arguments (given %d, expected 0..1)!", argc);
	}
	
	if (!NIL_P(rb_max_threads)) {
		max_threads = NUM2SIZET(rb_max_threads);
		if (max_threads == 0) {
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
	pool->threads = NULL;
	pool->thread_count = 0;
	pool->max_threads = max_threads;
	pool->shutdown = false;
	
	// Create initial worker threads
	for (size_t i = 0; i < max_threads; i++) {
		if (create_worker_thread(pool) != 0) {
			// Just set the max_threads for debugging, don't fail completely
			// worker_pool_free(pool);
			// rb_raise(rb_eRuntimeError, "Failed to create worker threads");
			break;
		}
	}
	
	return self;
}

// Ruby method to submit work
static VALUE worker_pool_call(VALUE self, VALUE work) {
	struct IO_Event_WorkerPool *pool;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	if (pool->shutdown) {
		rb_raise(rb_eRuntimeError, "Worker pool is shut down!");
	}
	
	// Extract blocking operation handle
	rb_fiber_scheduler_blocking_operation_t *blocking_operation = 
		rb_fiber_scheduler_blocking_operation_extract(work);
	
	if (!blocking_operation) {
		rb_raise(rb_eArgError, "Invalid blocking operation!");
	}
	
	// Create work item
	work_item_t *work_item = malloc(sizeof(work_item_t));
	if (!work_item) {
		rb_raise(rb_eNoMemError, "Failed to allocate work item!");
	}
	
	work_item->blocking_operation = blocking_operation;
	work_item->cancelled = false;
	work_item->next = NULL;
	
	// Create promise
	struct IO_Event_WorkerPool_Promise *promise_data;
	VALUE promise = TypedData_Make_Struct(IO_Event_WorkerPool_Promise, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise_data);
	
	promise_data->work_item = work_item;
	promise_data->pool = pool;
	promise_data->fiber = IO_Event_Fiber_current();
	promise_data->completed = false;
	promise_data->cancelled = false;
	
	work_item->promise = promise;
	
	// Enqueue work
	pthread_mutex_lock(&pool->mutex);
	
	if (pool->work_queue_tail) {
		pool->work_queue_tail->next = work_item;
	} else {
		pool->work_queue = work_item;
	}
	pool->work_queue_tail = work_item;
	
	pthread_cond_signal(&pool->work_available);
	pthread_mutex_unlock(&pool->mutex);
	
	return promise;
}

// Promise cancel method
static VALUE worker_pool_promise_cancel(VALUE self) {
	struct IO_Event_WorkerPool_Promise *promise;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise);
	
	if (promise->completed || promise->cancelled) {
		return Qfalse;
	}
	
	promise->cancelled = true;
	promise->work_item->cancelled = true;
	
	// Try to cancel the blocking operation
	if (promise->work_item->blocking_operation) {
		rb_fiber_scheduler_blocking_operation_cancel(promise->work_item->blocking_operation);
	}
	
	return Qtrue;
}

// Promise cancelled? predicate
static VALUE worker_pool_promise_cancelled_p(VALUE self) {
	struct IO_Event_WorkerPool_Promise *promise;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise);
	
	return promise->cancelled ? Qtrue : Qfalse;
}

// Promise completed? predicate  
static VALUE worker_pool_promise_completed_p(VALUE self) {
	struct IO_Event_WorkerPool_Promise *promise;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise);
	
	return promise->completed ? Qtrue : Qfalse;
}

// Promise wait method
static VALUE worker_pool_promise_wait(VALUE self) {
	struct IO_Event_WorkerPool_Promise *promise;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise);
	
	if (promise->completed) {
		return self;
	}
	
	if (promise->cancelled) {
		rb_raise(rb_eRuntimeError, "Operation was cancelled!");
	}
	
	struct IO_Event_WorkerPool *pool = promise->pool;
	
	// Wait for completion
	pthread_mutex_lock(&pool->mutex);
	while (!promise->completed && !promise->cancelled) {
		pthread_cond_wait(&pool->work_completed, &pool->mutex);
	}
	pthread_mutex_unlock(&pool->mutex);
	
	if (promise->cancelled) {
		rb_raise(rb_eRuntimeError, "Operation was cancelled!");
	}
	
	return self;
}

static VALUE worker_pool_allocate(VALUE klass) {
	struct IO_Event_WorkerPool *pool;
	VALUE obj = TypedData_Make_Struct(klass, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	// Initialize to NULL/zero so we can detect uninitialized pools
	memset(pool, 0, sizeof(struct IO_Event_WorkerPool));
	
	return obj;
}

static VALUE worker_pool_promise_allocate(VALUE klass) {
	struct IO_Event_WorkerPool_Promise *promise;
	return TypedData_Make_Struct(klass, struct IO_Event_WorkerPool_Promise, &IO_Event_WorkerPool_Promise_type, promise);
}

// Test helper: get pool statistics for debugging/testing
static VALUE worker_pool_statistics(VALUE self) {
	struct IO_Event_WorkerPool *pool;
	TypedData_Get_Struct(self, struct IO_Event_WorkerPool, &IO_Event_WorkerPool_type, pool);
	
	if (!pool) {
		rb_raise(rb_eRuntimeError, "WorkerPool not initialized!");
	}
	
	VALUE stats = rb_hash_new();
	rb_hash_aset(stats, ID2SYM(rb_intern("thread_count")), SIZET2NUM(pool->thread_count));
	rb_hash_aset(stats, ID2SYM(rb_intern("max_threads")), SIZET2NUM(pool->max_threads));
	rb_hash_aset(stats, ID2SYM(rb_intern("shutdown")), pool->shutdown ? Qtrue : Qfalse);
	
	// Count work items in queue (only if properly initialized)
	if (pool->max_threads > 0) {
		pthread_mutex_lock(&pool->mutex);
		size_t queue_size = 0;
		work_item_t *work = pool->work_queue;
		while (work) {
			queue_size++;
			work = work->next;
		}
		pthread_mutex_unlock(&pool->mutex);
		rb_hash_aset(stats, ID2SYM(rb_intern("queue_size")), SIZET2NUM(queue_size));
	} else {
		rb_hash_aset(stats, ID2SYM(rb_intern("queue_size")), SIZET2NUM(0));
	}
	
	return stats;
}

void Init_IO_Event_WorkerPool(VALUE IO_Event) {
	IO_Event_WorkerPool = rb_define_class_under(IO_Event, "WorkerPool", rb_cObject);
	rb_define_alloc_func(IO_Event_WorkerPool, worker_pool_allocate);
	rb_define_method(IO_Event_WorkerPool, "initialize", worker_pool_initialize, -1);
	rb_define_method(IO_Event_WorkerPool, "call", worker_pool_call, 1);
	rb_define_method(IO_Event_WorkerPool, "statistics", worker_pool_statistics, 0);
	
	
	IO_Event_WorkerPool_Promise = rb_define_class_under(IO_Event_WorkerPool, "Promise", rb_cObject);
	rb_define_alloc_func(IO_Event_WorkerPool_Promise, worker_pool_promise_allocate);
	rb_define_method(IO_Event_WorkerPool_Promise, "cancel", worker_pool_promise_cancel, 0);
	rb_define_method(IO_Event_WorkerPool_Promise, "cancelled?", worker_pool_promise_cancelled_p, 0);
	rb_define_method(IO_Event_WorkerPool_Promise, "completed?", worker_pool_promise_completed_p, 0);
	rb_define_method(IO_Event_WorkerPool_Promise, "wait", worker_pool_promise_wait, 0);
}
