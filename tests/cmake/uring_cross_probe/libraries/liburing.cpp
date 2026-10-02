#include <cstdlib>
#include <liburing.h>

// Calling any target-side API fails. These tests may compile and link only.
extern "C" {
int io_uring_queue_init_params(unsigned, io_uring*, io_uring_params*) {
    std::abort();
}
void io_uring_queue_exit(io_uring*) { std::abort(); }
io_uring_sqe* io_uring_get_sqe(io_uring*) { std::abort(); }
void io_uring_prep_readv(io_uring_sqe*, int, const iovec*, unsigned,
                        unsigned long long) { std::abort(); }
int io_uring_submit(io_uring*) { std::abort(); }
int io_uring_peek_cqe(io_uring*, io_uring_cqe**) { std::abort(); }
int io_uring_wait_cqe(io_uring*, io_uring_cqe**) { std::abort(); }
unsigned io_uring_cq_ready(io_uring*) { std::abort(); }
void io_uring_cqe_seen(io_uring*, io_uring_cqe*) { std::abort(); }

#ifdef AASTORE_TEST_COMPLETE_LIBURING
void io_uring_prep_read(io_uring_sqe*, int, void*, unsigned,
                       unsigned long long) { std::abort(); }
io_uring_probe* io_uring_get_probe_ring(io_uring*) { std::abort(); }
int io_uring_opcode_supported(io_uring_probe*, int) { std::abort(); }
#endif
}
