// Released under the MIT License.
// Copyright, 2026, by Samuel Williams.

// Fallback for process_wait when pidfd_open(2) returns EPERM, e.g. inside snap
// confinement (pre-snapd 2.75). Uses signalfd(2) + SIGCHLD instead.
//
// Included (not compiled separately) by epoll.c and uring.c, like pidfd.c.

#ifdef HAVE_SYS_SIGNALFD_H
#include <sys/signalfd.h>
#include <signal.h>

// Block SIGCHLD for this thread and create a signalfd.
//
// If the process has already exited, stores the status in *result and returns -1.
// Otherwise returns the signalfd descriptor (>= 0).
static int
process_wait_signalfd_open(pid_t pid, int flags, sigset_t *old_mask, VALUE *result)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	pthread_sigmask(SIG_BLOCK, &mask, old_mask);

	int descriptor = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (descriptor == -1) {
		pthread_sigmask(SIG_SETMASK, old_mask, NULL);
		rb_sys_fail("process_wait_signalfd_open:signalfd");
	}
	rb_update_max_fd(descriptor);

	// Check if the process has already exited:
	*result = IO_Event_Selector_process_status_wait(pid, flags);
	if (*result != Qnil) {
		close(descriptor);
		pthread_sigmask(SIG_SETMASK, old_mask, NULL);
		return -1;
	}

	return descriptor;
}

// Drain the signalfd and check whether a specific process has exited.
//
// Returns the process status, or Qnil if it hasn't exited yet (the SIGCHLD was
// for a different child).
static VALUE
process_wait_signalfd_check(int descriptor, pid_t pid, int flags)
{
	struct signalfd_siginfo info;
	while (read(descriptor, &info, sizeof(info)) > 0) {}

	return IO_Event_Selector_process_status_wait(pid, flags);
}

// Close the signalfd and restore the original signal mask.
static void
process_wait_signalfd_close(int descriptor, sigset_t *old_mask)
{
	close(descriptor);
	pthread_sigmask(SIG_SETMASK, old_mask, NULL);
}

#endif
