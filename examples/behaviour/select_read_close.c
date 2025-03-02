#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>
#include <errno.h>
#include <string.h>

static int pipefd[2];

void* select_thread(void* arg) {
    fd_set readfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&readfds);
    FD_SET(pipefd[0], &readfds);

    /* Set a timeout, so select won't block forever if something unexpected happens. */
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    printf("Thread: calling select()...\n");

    ret = select(pipefd[0] + 1, &readfds, NULL, NULL, &tv);
    if (ret == -1) {
        /* You will often see errno = EBADF after the close() in the other thread. */
        printf("Thread: select() returned -1, errno=%d (%s)\n", errno, strerror(errno));
    } else if (ret == 0) {
        printf("Thread: select() timed out\n");
    } else {
        printf("Thread: select() returned %d (pipefd[0] is readable)\n", ret);
    }

    return NULL;
}

int main(void) {
    pthread_t tid;
    int ret;

    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    /* Create a thread that calls select() on the read end of the pipe. */
    ret = pthread_create(&tid, NULL, select_thread, NULL);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed: %s\n", strerror(ret));
        exit(EXIT_FAILURE);
    }

    /* Give the select() thread time to block. */
    sleep(1);

    /* Close the read end of the pipe from the main thread. */
    printf("Main: closing pipefd[0]...\n");
    close(pipefd[0]);

    /* Wait for the select thread to finish. */
    pthread_join(tid, NULL);

    /* Clean up the write end (though it's not strictly necessary here). */
    close(pipefd[1]);

    return 0;
}
