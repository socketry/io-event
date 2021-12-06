
// This demonstrates a race condiiton between kqueue and waidpid.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <spawn.h>

int main(int argc, char ** argv) {
	int result;
	char * arguments[] = {"sleep", "0.01", NULL};
	
	while (1) {
		int pid = 0;
		result = posix_spawn(&pid, "/bin/sleep", NULL, NULL, arguments, NULL);
		fprintf(stderr, "posix_spawn result=%d\n", result);
		if (result) {
			perror("posix_spawn");
			exit(result);
		}
		
		int fd = kqueue();
		struct kevent kev;
		EV_SET(&kev, pid, EVFILT_PROC, EV_ADD|EV_ENABLE, NOTE_EXIT, 0, NULL);
		kevent(fd, &kev, 1, NULL, 0, NULL);
		
		kevent(fd, NULL, 0, &kev, 1, NULL); // wait
		
		int status = -1;
		result = waitpid(pid, &status, WNOHANG);
		fprintf(stderr, "waitpid(%d) result=%d status=%d\n", pid, result, status);
		
		if (status) {
			exit(status);
		}
		
		close(fd);
	}
}
