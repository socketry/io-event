/*
 * poll_zero.c — bare io_uring stress test for the ENOTCONN / ZERO poll result bug.
 *
 * Two modes:
 *
 *  MODE 0 (default) — normal poll:
 *    Connect → poll_add(POLLIN|POLLOUT|POLLERR|POLLHUP) → collect CQE.
 *    Checks for ZERO: (cqe->res & mask)==0 while cqe->res > 0.
 *
 *  MODE 1 (--cancel) — cancel race, mirrors io_wait_ensure in io-event:
 *    Connect → poll_add → immediately submit prep_cancel → collect both CQEs.
 *    The poll CQE may arrive as -ECANCELED (negative, normal) or with a positive
 *    result if the connect raced the cancel.  Any ZERO result here would be
 *    evidence the cancel path can trigger the bug.
 *
 * Build:  cc -O2 -o poll_zero poll_zero.c -luring
 * Run:    ./poll_zero [concurrency [total [--cancel]]]
 * Exits 1 if any ZERO result is observed.
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

#define REQUESTED_MASK  (POLLIN | POLLOUT | POLLERR | POLLHUP)   /* 0x001d */
#define RING_SIZE       512

/* Tag bits packed into user_data so we can distinguish poll vs cancel CQEs. */
#define TAG_POLL    0x0000000000000000ULL
#define TAG_CANCEL  0x8000000000000000ULL
#define FD_MASK     0x7FFFFFFFFFFFFFFFULL

static void *acceptor(void *arg) {
    int srv = *(int *)arg;
    while (1) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINVAL || errno == EBADF) break; continue; }
        close(fd);
    }
    return NULL;
}

static int make_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) { io_uring_submit(ring); sqe = io_uring_get_sqe(ring); }
    return sqe;
}

static void process_cqe(struct io_uring_cqe *cqe, int cancel_mode,
                        long *ok, long *unexpected, long *zero,
                        long *errors, long *cancelled) {
    uint64_t ud  = io_uring_cqe_get_data64(cqe);
    int is_cancel = !!(ud & TAG_CANCEL);
    int32_t res  = cqe->res;

    if (is_cancel) {
        /* cancel completion — res=-EALREADY (already fired) or 0 (cancelled ok) */
        (*cancelled)++;
        return;
    }

    /* poll completion */
    if (res < 0) {
        /* -ECANCELED, -EBADF etc. — negative, handled by result<0 branch in io-event */
        (*errors)++;
    } else if (res == 0) {
        /* Qfalse equivalent — would raise IOTimeoutError in Ruby */
        (*errors)++;
    } else {
        /* res > 0 — the interesting case */
        unsigned unexpected_bits = (unsigned)res & ~(unsigned)REQUESTED_MASK;
        unsigned matched         = (unsigned)res &  (unsigned)REQUESTED_MASK;

        if (matched == 0) {
            (*zero)++;
            fprintf(stderr, "ZERO: result=%#010x flags=%#010x%s\n",
                    (unsigned)res, (unsigned)REQUESTED_MASK,
                    cancel_mode ? " [cancel-mode]" : "");
        } else {
            (*ok)++;
            if (unexpected_bits) {
                (*unexpected)++;
                /* Only print first few to avoid flooding */
                static long printed = 0;
                if (printed++ < 5)
                    fprintf(stderr, "UNEXPECTED: result=%#010x unexpected=%#010x%s\n",
                            (unsigned)res, unexpected_bits,
                            cancel_mode ? " [cancel-mode]" : "");
            }
        }
    }
}

int main(int argc, char **argv) {
    int concurrency  = argc > 1 ? atoi(argv[1]) : 200;
    int total        = argc > 2 ? atoi(argv[2]) : 20000;
    int cancel_mode  = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--cancel") == 0) cancel_mode = 1;

    fprintf(stderr, "Mode: %s  concurrency=%d  total=%d\n",
            cancel_mode ? "cancel-race" : "normal-poll", concurrency, total);

    /* ── server ──────────────────────────────────────────────────────── */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 1024);
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);
    fprintf(stderr, "Server on 127.0.0.1:%d\n", ntohs(addr.sin_port));

    pthread_t acc_thread;
    pthread_create(&acc_thread, NULL, acceptor, &srv);

    /* ── io_uring ─────────────────────────────────────────────────────── */
    struct io_uring ring;
    struct io_uring_params params = {};
#ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
    params.flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
    if (io_uring_queue_init_params(RING_SIZE, &ring, &params) < 0) {
        memset(&params, 0, sizeof(params));
        io_uring_queue_init_params(RING_SIZE, &ring, &params);
    }
    fprintf(stderr, "io_uring flags=0x%x features=0x%x\n\n", ring.flags, ring.features);

    /* ── run ─────────────────────────────────────────────────────────── */
    long ok = 0, unexpected = 0, zero = 0, errors = 0, cancelled = 0;

    /*
     * In cancel mode we need to track each in-flight fd so we can submit
     * the cancel pointing at the poll user_data.  In normal mode we just
     * tag user_data with the fd.
     */
    int in_flight = 0, submitted = 0, completed = 0;
    /* expected completions per connection: 1 (poll) + 1 (cancel) if cancel_mode */
    int cqes_per_conn = cancel_mode ? 2 : 1;
    int total_cqes = total * cqes_per_conn;
    int cqes_received = 0;

    while (cqes_received < total_cqes) {
        /* Submit up to concurrency new connections */
        while (in_flight < concurrency && submitted < total) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { submitted++; errors++; continue; }
            make_nonblocking(fd);

            int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
            if (r != 0 && errno != EINPROGRESS) {
                close(fd); submitted++; errors++; continue;
            }

            /* poll_add — user_data = TAG_POLL | fd */
            uint64_t poll_ud = TAG_POLL | (uint64_t)fd;
            struct io_uring_sqe *sqe = get_sqe(&ring);
            io_uring_prep_poll_add(sqe, fd, REQUESTED_MASK);
            io_uring_sqe_set_data64(sqe, poll_ud);

            if (cancel_mode) {
                /*
                 * Immediately submit a cancel targeting this poll's user_data.
                 * user_data of the cancel SQE = TAG_CANCEL | fd so we can
                 * identify the cancel CQE.
                 * The cancel targets user_data = poll_ud.
                 */
                struct io_uring_sqe *csqe = get_sqe(&ring);
                io_uring_prep_cancel64(csqe, poll_ud, 0);
                io_uring_sqe_set_data64(csqe, TAG_CANCEL | (uint64_t)fd);
            }

            in_flight++;
            submitted++;
        }

        io_uring_submit(&ring);

        /* Drain CQEs */
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) { fprintf(stderr, "wait_cqe: %s\n", strerror(-ret)); break; }

        do {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            int is_cancel_cqe = !!(ud & TAG_CANCEL);
            int fd_done = (int)(ud & FD_MASK);

            process_cqe(cqe, cancel_mode, &ok, &unexpected, &zero, &errors, &cancelled);
            io_uring_cqe_seen(&ring, cqe);
            cqes_received++;

            /* Close fd and decrement in_flight after the poll CQE (not the cancel CQE) */
            if (!is_cancel_cqe) {
                close(fd_done);
                in_flight--;
                completed++;
            }
        } while (!io_uring_peek_cqe(&ring, &cqe));
    }

    io_uring_queue_exit(&ring);
    close(srv);
    pthread_cancel(acc_thread);
    pthread_join(acc_thread, NULL);

    fprintf(stderr, "\n=== Results (%d connections, mode=%s) ===\n",
            total, cancel_mode ? "cancel-race" : "normal-poll");
    fprintf(stderr, "  ok (matched)  : %ld\n", ok);
    fprintf(stderr, "  unexpected    : %ld  (extra bits, but match non-zero)\n", unexpected);
    fprintf(stderr, "  ZERO          : %ld  *** Integer(0) path ***\n", zero);
    fprintf(stderr, "  errors/neg    : %ld\n", errors);
    if (cancel_mode)
        fprintf(stderr, "  cancel CQEs   : %ld\n", cancelled);

    return zero > 0 ? 1 : 0;
}
