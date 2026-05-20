/*
 * poll_zero.c — systematic io_uring permutation stress test.
 *
 * Searches for the condition that causes cqe->res > 0 with
 * (cqe->res & POLLIN|POLLOUT|POLLERR|POLLHUP) == 0 (the ZERO case),
 * which makes io_wait return Integer(0) and leaves the socket in
 * TCP_SYN_SENT, causing ENOTCONN from getpeername(2).
 *
 * Modes:
 *   --flags FLAGS     io_uring setup flags (hex, e.g. 0x3200)
 *   --ring N          SQ ring size (default 512; io-event uses 64)
 *   --cancel          race io_uring_prep_cancel against every poll_add
 *   --overflow        submit 2x ring-size polls to force CQ overflow
 *   --remote HOST:P   connect to remote host (real RTT)
 *
 * Build:  cc -O2 -o poll_zero poll_zero.c -luring -lpthread
 * Run:    ./poll_zero [concurrency [total]] [options...]
 * Exit 1 if ZERO observed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <liburing.h>

#define REQUESTED_MASK  (POLLIN | POLLOUT | POLLERR | POLLHUP)
#define TAG_CANCEL      0x8000000000000000ULL
#define FD_MASK         0x7FFFFFFFFFFFFFFFULL

/* ── default io_uring flags matching io-event ────────────────────────── */
static unsigned default_flags(void) {
    unsigned f = 0;
#ifdef IORING_SETUP_SINGLE_ISSUER
    f |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
    f |= IORING_SETUP_DEFER_TASKRUN;
#endif
#ifdef IORING_SETUP_TASKRUN_FLAG
    f |= IORING_SETUP_TASKRUN_FLAG;
#endif
    return f;
}

/* ── acceptor ────────────────────────────────────────────────────────── */
static void *acceptor(void *arg) {
    int srv = *(int *)arg;
    while (1) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINVAL || errno == EBADF) break; continue; }
        close(fd);
    }
    return NULL;
}

static int make_nb(int fd) { return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL,0) | O_NONBLOCK); }

static struct io_uring_sqe *get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *s = io_uring_get_sqe(ring);
    if (!s) { io_uring_submit(ring); s = io_uring_get_sqe(ring); }
    return s;
}

/* ── run one scenario ────────────────────────────────────────────────── */
static int run(const char *label, struct io_uring *ring,
               struct sockaddr_in *target, int total, int concurrency,
               int cancel_mode, int overflow_mode, int epollet_mode)
{
    long ok = 0, unexp = 0, zero = 0, errs = 0, cancels = 0;
    int in_flight = 0, submitted = 0, cqes_recv = 0;
    int cqes_expected = total * (cancel_mode ? 2 : 1);
    if (overflow_mode) cqes_expected = total;  /* overflow drops some */

    while (cqes_recv < cqes_expected) {
        int batch = overflow_mode ? concurrency * 3 : concurrency;
        while (in_flight < batch && submitted < total) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) { submitted++; errs++; continue; }
            make_nb(fd);
            int r = connect(fd, (struct sockaddr *)target, sizeof(*target));
            if (r != 0 && errno != EINPROGRESS) {
                close(fd); submitted++; errs++; continue;
            }
            uint64_t poll_ud = (uint64_t)fd;
            struct io_uring_sqe *sqe = get_sqe(ring);
            /* --epollet simulates COS kernel patch to io_poll_parse_events
             * that unconditionally adds EPOLLET to every non-level poll.   */
            unsigned poll_mask = REQUESTED_MASK | (epollet_mode ? EPOLLET : 0);
            io_uring_prep_poll_add(sqe, fd, poll_mask);
            io_uring_sqe_set_data64(sqe, poll_ud);
            if (cancel_mode) {
                struct io_uring_sqe *cs = get_sqe(ring);
                io_uring_prep_cancel64(cs, poll_ud, 0);
                io_uring_sqe_set_data64(cs, TAG_CANCEL | (uint64_t)fd);
            }
            in_flight++; submitted++;
        }
        io_uring_submit(ring);

        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(ring, &cqe) < 0) break;
        do {
            uint64_t ud = io_uring_cqe_get_data64(cqe);
            int is_cancel = !!(ud & TAG_CANCEL);
            int fd_done = (int)(ud & FD_MASK);
            int32_t res = cqe->res;
            io_uring_cqe_seen(ring, cqe);
            cqes_recv++;
            if (is_cancel) { cancels++; goto next; }
            if (res <= 0) { errs++; }
            else {
                unsigned unmatched = (unsigned)res & ~(unsigned)REQUESTED_MASK;
                unsigned matched   = (unsigned)res &  (unsigned)REQUESTED_MASK;
                if (matched == 0) {
                    zero++;
                    fprintf(stderr, "  *** ZERO: res=%#010x label=%s\n", (unsigned)res, label);
                } else {
                    ok++;
                    if (unmatched) unexp++;
                }
            }
            if (!is_cancel) { close(fd_done); in_flight--; }
next:;
        } while (!io_uring_peek_cqe(ring, &cqe));
    }

    printf("%-42s  ok=%-5ld unexp=%-5ld ZERO=%-3ld err=%-5ld cancel=%-5ld\n",
           label, ok, unexp, zero, errs, cancels);
    return zero > 0 ? 1 : 0;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    int concurrency = 50, total = 2000;
    int cancel_mode = 0, overflow_mode = 0, epollet_mode = 0;
    int ring_size = 64;                  /* io-event 1.14.0 uses ring size 64 */
    unsigned custom_flags = 0xFFFFFFFF;  /* sentinel = use default */
    const char *remote_host = NULL; int remote_port = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--cancel"))                   cancel_mode = 1;
        else if (!strcmp(argv[i], "--overflow"))                 overflow_mode = 1;
        else if (!strcmp(argv[i], "--epollet"))                  epollet_mode = 1;
        else if (!strcmp(argv[i], "--ring") && i+1 < argc)       ring_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flags") && i+1 < argc)      custom_flags = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--remote") && i+1 < argc) {
            char *col = strrchr(argv[i+1], ':');
            if (!col) { fprintf(stderr, "--remote HOST:PORT\n"); return 1; }
            *col = '\0'; remote_host = argv[i+1]; remote_port = atoi(col+1); i++;
        }
        else if (atoi(argv[i]) > 0) {
            if (i == 1) concurrency = atoi(argv[i]);
            else if (i == 2) total = atoi(argv[i]);
        }
    }

    /* ── server ─────────────────────────────────────────────────── */
    struct sockaddr_in target = { .sin_family = AF_INET };
    pthread_t acc_thread = 0; int srv_fd = -1;

    if (remote_host) {
        struct addrinfo hints = {.ai_family=AF_INET,.ai_socktype=SOCK_STREAM};
        struct addrinfo *res;
        if (getaddrinfo(remote_host, NULL, &hints, &res) != 0) { perror("getaddrinfo"); return 1; }
        target = *(struct sockaddr_in *)res->ai_addr;
        target.sin_port = htons(remote_port);
        freeaddrinfo(res);
        printf("Remote: %s:%d  concurrency=%d  total=%d\n", remote_host, remote_port, concurrency, total);
    } else {
        srv_fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(srv_fd, (struct sockaddr *)&target, sizeof(target));
        listen(srv_fd, 4096);
        socklen_t al = sizeof(target); getsockname(srv_fd, (struct sockaddr *)&target, &al);
        printf("Local server 127.0.0.1:%d  concurrency=%d  total=%d\n",
               ntohs(target.sin_port), concurrency, total);
        pthread_create(&acc_thread, NULL, acceptor, &srv_fd);
    }

    unsigned def = default_flags();
    printf("\nKernel: "); fflush(stdout);
    system("uname -r");

    /* ── flag permutations ──────────────────────────────────────── */
    /*
     * Production sidekick uses io-event 1.14.0 which calls
     * io_uring_queue_init(N, ring, 0) — zero flags.
     * Current io-event uses SINGLE_ISSUER|DEFER_TASKRUN.
     * We test both, plus the COS EPOLLET variant of each.
     */
    struct { unsigned flags; const char *name; } configs[] = {
        { 0,   "flags=0  [io-event 1.14.0 / sidekick production]" },
        { def, "flags=default (SINGLE_ISSUER|DEFER_TASKRUN|TASKRUN_FLAG)" },
    };

    int ring_sizes[] = { ring_size };
    int any_zero = 0;

    for (int ri = 0; ri < (int)(sizeof ring_sizes / sizeof *ring_sizes); ri++) {
        int rs = ring_sizes[ri];

        for (int ci = 0; ci < (int)(sizeof configs / sizeof *configs); ci++) {
            unsigned f = (custom_flags != 0xFFFFFFFF) ? custom_flags : configs[ci].flags;

            struct io_uring ring;
            struct io_uring_params params = { .flags = f };
            if (io_uring_queue_init_params(rs, &ring, &params) < 0) {
                printf("  [skip: ring=%d flags=%#x]\n", rs, f);
                continue;
            }

            char label[160];
            snprintf(label, sizeof label, "ring=%-3d flags=0x%05x [%s%s%s] %s",
                     rs, ring.flags,
                     cancel_mode   ? "cancel "   : "",
                     overflow_mode ? "overflow " : "",
                     epollet_mode  ? "epollet "  : "",
                     (custom_flags != 0xFFFFFFFF) ? "custom" : configs[ci].name);

            any_zero |= run(label, &ring, &target, total, concurrency,
                            cancel_mode, overflow_mode, epollet_mode);

            io_uring_queue_exit(&ring);

            if (custom_flags != 0xFFFFFFFF) break;
        }
    }

    if (srv_fd >= 0) { close(srv_fd); pthread_cancel(acc_thread); pthread_join(acc_thread, NULL); }

    printf("\n%s\n", any_zero ? "ZERO OBSERVED — root cause confirmed!" : "No ZERO observed.");
    return any_zero;
}
