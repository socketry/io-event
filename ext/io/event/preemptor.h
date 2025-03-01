// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <signal.h>

#include <time.h>
#include <sys/time.h>

typedef void (*IO_Event_Preemptor_Callback)(void* user_data);

struct IO_Event_Preemptor {
#if defined(__linux__)
	// Used with timer_create on Linux:
	timer_t timer;
#else
	// Used with setitimer on macOS:
	struct itimerval timer;
#endif
	
	int signal;
	
	IO_Event_Preemptor_Callback callback;
	void* user_data;
};

int IO_Event_Preemptor_initialize(struct IO_Event_Preemptor* preemptor, int signal, IO_Event_Preemptor_Callback callback, void* user_data);
void IO_Event_Preemptor_free(struct IO_Event_Preemptor* preemptor);
int IO_Event_Preemptor_start(struct IO_Event_Preemptor* preemptor, long timeout_ms);
int IO_Event_Preemptor_stop(struct IO_Event_Preemptor* preemptor);

// Create a new IO_Event_Preemptor instance suitable for the current platform. On Linux, it uses SIGRTMIN + 1 if available. On macOS, it defaults to SIGALRM. Returns 0 on success or -1 on error.
int IO_Event_Preemptor_default(struct IO_Event_Preemptor* preemptor, IO_Event_Preemptor_Callback callback, void* user_data);
