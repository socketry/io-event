/*
 * poll_zero.c — io_uring poll_add stress test for the ZERO result bug.
 *
 * Three server modes (select one):
 *
 *   (default)         local server, accepts immediately — loopback RTT ~0ms
 *   --delay MS        local server sleeps MS milliseconds before accept,
 *                     keeping the socket in TCP_SYN_SENT long enough for
 *                     races to develop (simulates real network latency)
 *   --remote HOST:PORT connect to a real remote host with genuine RTT
 *
 * Two poll modes (combine freely):
 *
 *   (default)         poll_add only
 *   --cancel          immediately submit prep_cancel after each poll_add,
 *                     mirroring io_wait_ensure in io-event
 *
 * Every completed poll CQE is checked:
 *   UNEXPECTED  cqe->res has bits outside POLLIN|POLLOUT|POLLERR|POLLHUP
 *   ZERO        (cqe->res & mask)==0 while cqe->res > 0  ← Integer(0) path
 *
 * Build:   cc -O2 -o poll_zero poll_zero.c -luring -lpthread
 * Run:     ./poll_zero [concurrency [total]] [--cancel] [--delay MS] [--remote H:P]
 * Exit 1 if any ZERO result observed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <liburing.h>

#define REQUESTED_MASK  (POLLIN | POLLOUT | POLLERR | POLLHUP)
#define RING_SIZE       512

#define TAG_CANCEL  0x8000000000000000ULL
#define FD_MASK     0x7FFFFFFFFFFFFFFFULL

/* ── acceptor ──────────────────────────────────────────────────────────── */

struct srv_args { int fd; int delay_ms; };

static void *acceptor(void *arg) {
    struct srv_args *a = arg;
    while (1) {
        int fd = accept(a->fd, NULL, NULL);
        if (fd < 0) { if (errno == EINVAL || errno == EBADF) break; continue; }
        if (a->delay_ms > 0) {
            struct timespec ts = { .tv_sec  = a->delay_ms / 1000,
                                   .tv_nsec = (a->delay_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        close(fd);
    }
    return NULL;
}

/* ── helpers ────────────────────────────────────────────────────────────── */

static int make_nonblocking(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *s = io_uring_get_sqe(ring);
    if (!s) { io_uring_submit(ring); s = io_uring_get_sqe(ring); }
    return s;
}

/* ── CQE analysis ───────────────────────────────────────────────────────── */

static void process_cqe(struct io_uring_cqe *cqe, int cancel_mode,
                        long *ok, long *unexp, long *zero,
                        long *errs, long *cancels)
{
    uint64_t ud = io_uring_cqe_get_data64(cqe);
    if (ud & TAG_CANCEL) { (*cancels)++; return; }

    int32_t res = cqe->res;
    if (res <= 0) { (*errs)++; return; }

    unsigned unmatched = (unsigned)res & ~(unsigned)REQUESTED_MASK;
    unsigned matched   = (unsigned)res &  (unsigned)REQUESTED_MASK;

    if (matched == 0) {
        (*zero)++;
        fprintf(stderr, "ZERO: cqe->res=%#010x requested=%#010x%s\n",
                (unsigned)res, (unsigned)REQUESTED_MASK,
                cancel_mode ? " [cancel]" : "");
    } else {
        (*ok)++;
        if (unmatched) {
            (*unexp)++;
            static long printed;
            if (printed++ < 5)
                fprintf(stderr, "UNEXPECTED: res=%#010x unmatched=%#010x%s\n",
                        (unsigned)res, unmatched,
                        cancel_mode ? " [cancel]" : "");
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int concurrency = 200, total = 20000;
    int cancel_mode = 0, delay_ms = 0;
    const char *remote_host = NULL;
    int remote_port = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--cancel"))         cancel_mode = 1;
        else if (!strcmp(argv[i], "--delay") && i+1 < argc) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--remote") && i+1 < argc) {
            char *colon = strrchr(argv[i+1], ':');
            if (!colon) { fprintf(stderr, "--remote needs HOST:PORT\n"); return 1; }
            *colon = '\0';
            remote_host = argv[i+1];
            remote_port = atoi(colon + 1);
            i++;
        } else if (atoi(argv[i]) > 0) {
            if      (i == 1) concurrency = atoi(argv[i]);
            else if (i == 2) total       = atoi(argv[i]);
        }
    }

    /* ── server setup ───────────────────────────────────────────────── */
    struct sockaddr_in target = { .sin_family = AF_INET };
    pthread_t acc_thread;
    int srv_fd = -1;

    if (remote_host) {
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res;
        if (getaddrinfo(remote_host, NULL, &hints, &res) != 0) {
            perror("getaddrinfo"); return 1;
        }
        target = *(struct sockaddr_in *)res->ai_addr;
        target.sin_port = htons(remote_port);
        freeaddrinfo(res);
        fprintf(stderr, "Remote: %s:%d  concurrency=%d  total=%d\n",
                remote_host, remote_port, concurrency, total);
    } else {
        srv_fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(srv_fd, (struct sockaddr *)&target, sizeof(target));
        listen(srv_fd, 1024);
        socklen_t al = sizeof(target);
        getsockname(srv_fd, (struct sockaddr *)&target, &al);
        fprintf(stderr, "Local server 127.0.0.1:%d  delay=%dms  concurrency=%d  total=%d\n",
                ntohs(target.sin_port), delay_ms, concurrency, total);
        struct srv_args sargs = { .fd = srv_fd, .delay_ms = delay_ms };
        pthread_create(&acc_thread, NULL, acceptor, &sargs);
    }

    /* ── io_uring setup ─────────────────────────────────────────────── */
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
    fprintf(stderr, "io_uring flags=0x%x  cancel=%d\n\n", ring.flags, cancel_mode);

    /* ── event loop ─────────────────────────────────────────────────── */
    long ok = 0, unexp = 0, zero = 0, errs = 0, cancels = 0;
    int in_flight = 0, submitted = 0, cqes_recv = 0;
    int cqes_expected = total * (cancel_mode ? 2 : 1);

    while (cqes_recv < cqes_expected) {
        while (in_flight < concurrency && submitted < total) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { submitted++; errs++; continue; }
            make_nonblocking(fd);

            int r = connect(fd, (struct sockaddr *)&target, sizeof(target));
            if (r != 0 && errno != EINPROGRESS) {
                close(fd); submitted++; errs++; continue;
            }

            uint64_t poll_ud = (uint64_t)fd;   /* no TAG_POLL bit */
            struct io_uring_sqe *sqe = get_sqe(&ring);
            io_uring_prep_poll_add(sqe, fd, REQUESTED_MASK);
            io_uring_sqe_set_data64(sqe, poll_ud);

            if (cancel_mode) {
                struct io_uring_sqe *csqe = get_sqe(&ring);
                io_uring_prep_cancel64(csqe, poll_ud, 0);
                io_uring_sqe_set_data64(csqe, TAG_CANCEL | (uint64_t)fd);
            }

            in_flight++;
            submitted++;
        }

        io_uring_submit(&ring);

        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) break;

        do {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            int is_cancel = !!(ud & TAG_CANCEL);
            int fd_done = (int)(ud & FD_MASK);

            process_cqe(cqe, cancel_mode, &ok, &unexp, &zero, &errs, &cancels);
            io_uring_cqe_seen(&ring, cqe);
            cqes_recv++;

            if (!is_cancel) { close(fd_done); in_flight--; }
        } while (!io_uring_peek_cqe(&ring, &cqe));
    }

    io_uring_queue_exit(&ring);
    if (srv_fd >= 0) { close(srv_fd); pthread_cancel(acc_thread); pthread_join(acc_thread, NULL); }

    const char *mode = cancel_mode ? "cancel-race" : "poll-only";
    fprintf(stderr, "\n=== %s  %s  total=%d ===\n",
            remote_host ? remote_host : "localhost", mode, total);
    fprintf(stderr, "  ok (matched flags)  : %ld\n",  ok);
    fprintf(stderr, "  unexpected (RDHUP+) : %ld\n",  unexp);
    fprintf(stderr, "  ZERO *** root cause : %ld\n",  zero);
    fprintf(stderr, "  errors/negative     : %ld\n",  errs);
    if (cancel_mode)
        fprintf(stderr, "  cancel CQEs         : %ld\n", cancels);

    return zero > 0 ? 1 : 0;
}
