#ifndef AASTORE_TEST_CROSS_PROBE_LIBURING_H
#define AASTORE_TEST_CROSS_PROBE_LIBURING_H

#include <sys/uio.h>

enum { IORING_OP_READV = 1, IORING_OP_READ = 22 };
struct io_uring_sqe {
    unsigned opcode;
    unsigned long long user_data;
};
struct io_uring_cqe {
    unsigned long long user_data;
    int res;
};
struct io_uring_sq { io_uring_sqe* sqes; };
struct io_uring_cq { io_uring_cqe* cqes; };
struct io_uring { io_uring_sq sq; io_uring_cq cq; int ring_fd; };
struct io_uring_params { unsigned flags; };
struct io_uring_probe { unsigned operations; };

// These declarations deliberately require linked definitions. No probe runs.
extern "C" {
int io_uring_queue_init_params(unsigned, io_uring*, io_uring_params*);
void io_uring_queue_exit(io_uring*);
io_uring_sqe* io_uring_get_sqe(io_uring*);
void io_uring_prep_readv(io_uring_sqe*, int, const iovec*, unsigned,
                        unsigned long long);
int io_uring_submit(io_uring*);
int io_uring_peek_cqe(io_uring*, io_uring_cqe**);
int io_uring_wait_cqe(io_uring*, io_uring_cqe**);
unsigned io_uring_cq_ready(io_uring*);
void io_uring_cqe_seen(io_uring*, io_uring_cqe*);
void io_uring_prep_read(io_uring_sqe*, int, void*, unsigned,
                       unsigned long long);
io_uring_probe* io_uring_get_probe_ring(io_uring*);
int io_uring_opcode_supported(io_uring_probe*, int);
}

#endif
