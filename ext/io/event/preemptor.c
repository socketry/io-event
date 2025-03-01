// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "preemptor.h"

#include <memory.h>

// For SYS_gettid
#include <sys/syscall.h>

#include <stdio.h>
#include <unistd.h>

static void signal_handler(int signo, siginfo_t* info, void* context) {
	write(2, "signal_handler\n", 15);
	
	struct IO_Event_Preemptor* preemptor = (struct IO_Event_Preemptor*)info->si_value.sival_ptr;
	
	if (preemptor && preemptor->callback) {
		preemptor->callback(preemptor->user_data);
	}
}

int IO_Event_Preemptor_initialize(struct IO_Event_Preemptor* preemptor, int signal, IO_Event_Preemptor_Callback callback, void* user_data) {
	if (!preemptor || !callback) {
		return -1;
	}

	preemptor->signal = signal;
	preemptor->callback = callback;
	preemptor->user_data = user_data;

	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = signal_handler;
	sigemptyset(&sa.sa_mask);

	if (sigaction(signal, &sa, NULL) == -1) {
		perror("sigaction");
		return -1;
	}

	return 0;
}

void IO_Event_Preemptor_free(struct IO_Event_Preemptor* preemptor) {
#if defined(__linux__)
	if (preemptor && preemptor->timer) {
		timer_delete(preemptor->timer);
		preemptor->timer = 0;
	}
#else
	// No special cleanup needed for setitimer
	IO_Event_Preemptor_stop(preemptor);
#endif
}

int IO_Event_Preemptor_start(struct IO_Event_Preemptor* preemptor, long timeout_ms) {
	fprintf(stderr, "IO_Event_Preemptor_start\n");
	
	if (!preemptor || timeout_ms <= 0) {
		return -1;
	}
	
#if defined(__linux__)
	struct sigevent sev;
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = preemptor->signal;
	sev._sigev_un._tid = syscall(SYS_gettid);  // Target the current thread
	sev.sigev_value.sival_ptr = preemptor;
	
	if (timer_create(CLOCK_MONOTONIC, &sev, &preemptor->timer) == -1) {
		perror("timer_create");
		return -1;
	}
	
	struct itimerspec its;
	its.it_value.tv_sec = timeout_ms / 1000;
	its.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	
	if (timer_settime(preemptor->timer, 0, &its, NULL) == -1) {
		perror("timer_settime");
		timer_delete(preemptor->timer);
		return -1;
	}
#else
	struct itimerval its;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = timeout_ms / 1000;
	its.it_value.tv_usec = (timeout_ms % 1000) * 1000;
	
	if (setitimer(ITIMER_REAL, &its, NULL) == -1) {
		perror("setitimer");
		return -1;
	}
#endif
	
	return 0;
}

int IO_Event_Preemptor_stop(struct IO_Event_Preemptor* preemptor) {
	fprintf(stderr, "IO_Event_Preemptor_stop\n");
	
	if (!preemptor) {
		return -1;
	}
	
#if defined(__linux__)
	if (!preemptor->timer) {
		return -1;
	}
	
	struct itimerspec its = {0};
	if (timer_settime(preemptor->timer, 0, &its, NULL) == -1) {
		perror("timer_settime (stop)");
		return -1;
	}
#else
	struct itimerval its = {0};
	if (setitimer(ITIMER_REAL, &its, NULL) == -1) {
		perror("setitimer (stop)");
		return -1;
	}
#endif
	
	return 0;
}

int IO_Event_Preemptor_default(struct IO_Event_Preemptor* preemptor, IO_Event_Preemptor_Callback callback, void* user_data) {
	int signal;

#if defined(__linux__)
	// Use a real-time signal on Linux
	signal = SIGRTMIN + 1;
#else
	// Fallback to SIGALRM on macOS or other platforms
	signal = SIGALRM;
#endif

	return IO_Event_Preemptor_initialize(preemptor, signal, callback, user_data);
}
