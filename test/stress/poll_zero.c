/*
 * poll_zero.c — bare io_uring stress test for the ENOTCONN / ZERO poll result bug.
 *
 * Connects N sockets concurrently to a local server and immediately submits
 * io_uring_prep_poll_add(POLLIN|POLLOUT|POLLERR|POLLHUP) for each.  Every CQE
 * is checked against the requested mask:
 *
 *   UNEXPECTED — cqe->res has bits outside the requested mask
 *   ZERO       — (cqe->res & requested_mask) == 0 while cqe->res > 0
 *                This is the state that causes io_wait to return Integer(0)
 *                and leave the socket in TCP_SYN_SENT → ENOTCONN from getpeername.
 *
 * Build:
 *   cc -O2 -o poll_zero poll_zero.c -luring
 *
 * Run:
 *   ./poll_zero [concurrency [total]]
 *
 * Exits 1 if any ZERO result is observed; 0 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <liburing.h>

#define REQUESTED_MASK  (POLLIN | POLLOUT | POLLERR | POLLHUP)  /* 0x001d */
#define RING_SIZE       256

/* Acceptor thread: accept and immediately close each connection. */
static void *acceptor(void *arg) {
    int srv = *(int *)arg;
    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = accept(srv, (struct sockaddr *)&cli, &len);
        if (fd < 0) {
            if (errno == EINVAL || errno == EBADF) break; /* server closed */
            continue;
        }
        close(fd);
    }
    return NULL;
}

static int make_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int main(int argc, char **argv) {
    int concurrency = argc > 1 ? atoi(argv[1]) : 100;
    int total       = argc > 2 ? atoi(argv[2]) : 10000;

    /* ── server ─────────────────────────────────────────────────────────── */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 512);
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    int port = ntohs(addr.sin_port);

    pthread_t acc_thread;
    pthread_create(&acc_thread, NULL, acceptor, &srv);
    fprintf(stderr, "Server on 127.0.0.1:%d\n", port);

    /* ── io_uring ─────────────────────────────────────────────────────── */
    struct io_uring ring;
    struct io_uring_params params = {};
    /* Mirror io-event's setup: SINGLE_ISSUER + DEFER_TASKRUN if available */
#ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
    params.flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
    if (io_uring_queue_init_params(RING_SIZE, &ring, &params) < 0) {
        /* Fall back to plain ring if flags not supported */
        memset(&params, 0, sizeof(params));
        io_uring_queue_init_params(RING_SIZE, &ring, &params);
    }
    fprintf(stderr, "io_uring flags=0x%x features=0x%x\n", ring.flags, ring.features);

    /* ── stats ────────────────────────────────────────────────────────── */
    long ok = 0, unexpected = 0, zero = 0, errors = 0;
    int fds[concurrency];
    memset(fds, -1, sizeof(fds));

    int submitted = 0, completed = 0, in_flight = 0;

    while (completed < total) {
        /* Fill up to concurrency in-flight connections */
        while (in_flight < concurrency && submitted < total) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { perror("socket"); exit(1); }
            make_nonblocking(fd);

            int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
            if (r == 0 || errno == EINPROGRESS) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    /* ring full — submit and retry */
                    io_uring_submit(&ring);
                    sqe = io_uring_get_sqe(&ring);
                }
                io_uring_prep_poll_add(sqe, fd, REQUESTED_MASK);
                /* store fd in user_data so we can close it later */
                io_uring_sqe_set_data64(sqe, (uint64_t)fd);
                fds[in_flight] = fd;
                in_flight++;
                submitted++;
            } else {
                close(fd);
                errors++;
                submitted++;
            }
        }

        io_uring_submit(&ring);

        /* Drain at least one CQE */
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "wait_cqe: %s\n", strerror(-ret));
            break;
        }

        do {
            int32_t res = cqe->res;
            int fd_done = (int)io_uring_cqe_get_data64(cqe);
            io_uring_cqe_seen(&ring, cqe);
            close(fd_done);
            in_flight--;
            completed++;

            if (res < 0) {
                errors++;
            } else if (res > 0) {
                unsigned unexpected_bits = (unsigned)res & ~(unsigned)REQUESTED_MASK;
                unsigned matched = (unsigned)res & (unsigned)REQUESTED_MASK;
                if (matched == 0) {
                    zero++;
                    fprintf(stderr, "ZERO: result=%#010x flags=%#010x\n",
                            (unsigned)res, (unsigned)REQUESTED_MASK);
                } else {
                    ok++;
                    if (unexpected_bits) {
                        unexpected++;
                        fprintf(stderr, "UNEXPECTED: result=%#010x flags=%#010x unexpected=%#010x\n",
                                (unsigned)res, (unsigned)REQUESTED_MASK, unexpected_bits);
                    }
                }
            } else {
                /* res == 0: Qfalse equivalent */
                errors++;
            }
        } while (!io_uring_peek_cqe(&ring, &cqe));
    }

    io_uring_queue_exit(&ring);
    close(srv);
    pthread_cancel(acc_thread);
    pthread_join(acc_thread, NULL);

    fprintf(stderr, "\n=== Results (%d connections) ===\n", total);
    fprintf(stderr, "  ok          : %ld\n", ok);
    fprintf(stderr, "  unexpected  : %ld  (extra bits in cqe->res, but matched bits non-zero)\n", unexpected);
    fprintf(stderr, "  ZERO        : %ld  *** Integer(0) path — root cause of ENOTCONN ***\n", zero);
    fprintf(stderr, "  errors      : %ld  (negative cqe->res or connect failure)\n", errors);

    return zero > 0 ? 1 : 0;
}
