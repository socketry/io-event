// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>

#ifdef HAVE_RB_FIBER_SCHEDULER_BLOCKING_OPERATION_EXTRACT
#include <ruby/fiber/scheduler.h>
#endif

void Init_IO_Event_WorkerPool(VALUE IO_Event); 
