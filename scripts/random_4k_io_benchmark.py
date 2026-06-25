#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path


CPP_SOURCE = r'''
#include <liburing.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__linux__) && !defined(O_DIRECT)
#define O_DIRECT 040000
#endif

namespace {

constexpr uint64_t kDefaultBlockSize = 4096;

struct Args {
    std::string file;
    uint64_t reads = 100000;
    uint64_t seed = 12345;
    uint32_t threads = 1;
    uint32_t depth = 64;
    uint64_t block_size = kDefaultBlockSize;
    std::string backend = "uring";
    bool direct = true;
    bool drop_page_cache = false;
    bool uring_sqpoll = false;
    bool uring_iopoll = false;
    bool register_file = true;
};

struct WorkerStats {
    uint64_t reads = 0;
    uint64_t bytes = 0;
};

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
private:
    int fd_;
};

uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

double elapsed_ms(std::chrono::steady_clock::time_point begin,
                  std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

uint64_t file_size(int fd) {
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        throw std::runtime_error(std::string("fstat failed: ") + std::strerror(errno));
    }
    return static_cast<uint64_t>(st.st_size);
}

void* aligned_alloc_or_throw(uint64_t align, uint64_t bytes) {
    void* ptr = nullptr;
    int rc = ::posix_memalign(&ptr, static_cast<size_t>(align), static_cast<size_t>(bytes));
    if (rc != 0) {
        throw std::runtime_error(std::string("posix_memalign failed: ") + std::strerror(rc));
    }
    return ptr;
}

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc < 2) {
        throw std::runtime_error("usage: random_4k_io_runner <pages.bin> [options]");
    }
    args.file = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-n" || a == "--num") {
            args.reads = std::stoull(need_value(a.c_str()));
        } else if (a == "--seed") {
            args.seed = std::stoull(need_value(a.c_str()));
        } else if (a == "--threads") {
            args.threads = static_cast<uint32_t>(std::stoul(need_value(a.c_str())));
        } else if (a == "--depth") {
            args.depth = static_cast<uint32_t>(std::stoul(need_value(a.c_str())));
        } else if (a == "--block-size") {
            args.block_size = std::stoull(need_value(a.c_str()));
        } else if (a == "--backend") {
            args.backend = need_value(a.c_str());
        } else if (a == "--buffered") {
            args.direct = false;
        } else if (a == "--drop-page-cache") {
            args.drop_page_cache = true;
        } else if (a == "--uring-sqpoll") {
            args.uring_sqpoll = true;
        } else if (a == "--uring-iopoll") {
            args.uring_iopoll = true;
        } else if (a == "--no-register-file") {
            args.register_file = false;
        } else {
            throw std::runtime_error("unknown option: " + a);
        }
    }
    if (args.reads == 0) throw std::runtime_error("--num must be > 0");
    if (args.threads == 0) throw std::runtime_error("--threads must be > 0");
    if (args.depth == 0) throw std::runtime_error("--depth must be > 0");
    if (args.block_size == 0 || (args.block_size & (args.block_size - 1)) != 0) {
        throw std::runtime_error("--block-size must be a power of two");
    }
    if (args.backend != "pread" && args.backend != "uring") {
        throw std::runtime_error("--backend must be pread or uring");
    }
    return args;
}

WorkerStats run_pread_worker(const Args& args,
                             int fd,
                             const std::vector<uint64_t>& offsets,
                             uint64_t begin,
                             uint64_t end) {
    WorkerStats stats;
    void* raw = aligned_alloc_or_throw(args.block_size, args.block_size);
    unsigned char* buf = static_cast<unsigned char*>(raw);
    for (uint64_t i = begin; i < end; ++i) {
        ssize_t got = ::pread(fd, buf, static_cast<size_t>(args.block_size),
                              static_cast<off_t>(offsets[i]));
        if (got != static_cast<ssize_t>(args.block_size)) {
            std::free(raw);
            if (got < 0) {
                throw std::runtime_error(std::string("pread failed: ") + std::strerror(errno));
            }
            throw std::runtime_error("short pread");
        }
        stats.reads += 1;
        stats.bytes += args.block_size;
    }
    std::free(raw);
    return stats;
}

WorkerStats run_uring_worker(const Args& args,
                             int fd,
                             const std::vector<uint64_t>& offsets,
                             uint64_t begin,
                             uint64_t end) {
    WorkerStats stats;
    if (begin >= end) return stats;

    io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    if (args.uring_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
    }
    if (args.uring_iopoll) {
        params.flags |= IORING_SETUP_IOPOLL;
    }

    io_uring ring;
    int rc = io_uring_queue_init_params(args.depth, &ring, &params);
    if (rc < 0) {
        throw std::runtime_error(std::string("io_uring_queue_init failed: ") + std::strerror(-rc));
    }

    bool registered_file = false;
    if (args.register_file) {
        int fds[1] = {fd};
        rc = io_uring_register_files(&ring, fds, 1);
        if (rc == 0) {
            registered_file = true;
        } else {
            io_uring_queue_exit(&ring);
            throw std::runtime_error(std::string("io_uring_register_files failed: ") + std::strerror(-rc));
        }
    }

    const uint64_t buffer_bytes = static_cast<uint64_t>(args.depth) * args.block_size;
    void* raw = aligned_alloc_or_throw(args.block_size, buffer_bytes);
    unsigned char* pool = static_cast<unsigned char*>(raw);

    uint64_t next = begin;
    uint64_t completed = begin;
    uint64_t inflight = 0;

    auto prep_one = [&](uint32_t slot) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) throw std::runtime_error("io_uring_get_sqe returned null");
        const int read_fd = registered_file ? 0 : fd;
        io_uring_prep_read(sqe, read_fd, pool + static_cast<uint64_t>(slot) * args.block_size,
                           static_cast<unsigned>(args.block_size),
                           static_cast<off_t>(offsets[next]));
        if (registered_file) {
            sqe->flags |= IOSQE_FIXED_FILE;
        }
        sqe->user_data = slot;
        ++next;
        ++inflight;
    };

    const uint64_t initial = std::min<uint64_t>(args.depth, end - begin);
    for (uint32_t slot = 0; slot < initial; ++slot) {
        prep_one(slot);
    }
    rc = io_uring_submit(&ring);
    if (rc < 0) {
        std::free(raw);
        io_uring_queue_exit(&ring);
        throw std::runtime_error(std::string("io_uring_submit failed: ") + std::strerror(-rc));
    }

    while (completed < end) {
        io_uring_cqe* cqe = nullptr;
        rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc < 0) {
            std::free(raw);
            io_uring_queue_exit(&ring);
            throw std::runtime_error(std::string("io_uring_wait_cqe failed: ") + std::strerror(-rc));
        }

        unsigned head = 0;
        unsigned seen = 0;
        unsigned prepared = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            if (cqe->res != static_cast<int>(args.block_size)) {
                std::string msg = cqe->res < 0
                    ? std::string("io_uring read failed: ") + std::strerror(-cqe->res)
                    : "short io_uring read";
                std::free(raw);
                io_uring_queue_exit(&ring);
                throw std::runtime_error(msg);
            }
            const uint32_t slot = static_cast<uint32_t>(cqe->user_data);
            ++completed;
            --inflight;
            ++seen;
            stats.reads += 1;
            stats.bytes += args.block_size;
            if (next < end) {
                prep_one(slot);
                ++prepared;
            }
        }
        io_uring_cq_advance(&ring, seen);
        if (prepared > 0) {
            rc = io_uring_submit(&ring);
            if (rc < 0) {
                std::free(raw);
                io_uring_queue_exit(&ring);
                throw std::runtime_error(std::string("io_uring_submit failed: ") + std::strerror(-rc));
            }
        }
        (void)inflight;
    }

    std::free(raw);
    io_uring_queue_exit(&ring);
    return stats;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        const int flags = O_RDONLY | (args.direct ? O_DIRECT : 0);
        Fd fd(::open(args.file.c_str(), flags));
        if (fd.get() < 0) {
            throw std::runtime_error(std::string("open failed: ") + std::strerror(errno));
        }

        const uint64_t bytes = file_size(fd.get());
        const uint64_t blocks = bytes / args.block_size;
        if (blocks == 0) {
            throw std::runtime_error("file is smaller than block size");
        }

        if (args.drop_page_cache) {
            (void)::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED);
        }

        auto prep_begin = std::chrono::steady_clock::now();
        std::vector<uint64_t> offsets;
        offsets.resize(args.reads);
        uint64_t rng = args.seed;
        for (uint64_t i = 0; i < args.reads; ++i) {
            offsets[i] = (splitmix64(rng) % blocks) * args.block_size;
        }
        auto prep_end = std::chrono::steady_clock::now();

        std::vector<WorkerStats> worker_stats(args.threads);
        std::vector<std::string> errors(args.threads);
        std::vector<std::thread> workers;
        workers.reserve(args.threads);

        auto io_begin = std::chrono::steady_clock::now();
        for (uint32_t t = 0; t < args.threads; ++t) {
            const uint64_t begin = args.reads * t / args.threads;
            const uint64_t end = args.reads * (t + 1) / args.threads;
            workers.emplace_back([&, t, begin, end]() {
                try {
                    if (args.backend == "pread") {
                        worker_stats[t] = run_pread_worker(args, fd.get(), offsets, begin, end);
                    } else {
                        worker_stats[t] = run_uring_worker(args, fd.get(), offsets, begin, end);
                    }
                } catch (const std::exception& e) {
                    errors[t] = e.what();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        auto io_end = std::chrono::steady_clock::now();

        for (const auto& error : errors) {
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
        }

        uint64_t total_reads = 0;
        uint64_t total_bytes = 0;
        for (const auto& s : worker_stats) {
            total_reads += s.reads;
            total_bytes += s.bytes;
        }

        const double prep_ms = elapsed_ms(prep_begin, prep_end);
        const double io_ms = elapsed_ms(io_begin, io_end);
        const double sec = io_ms / 1000.0;
        const double iops = total_reads / sec;
        const double mbps = (static_cast<double>(total_bytes) / 1000000.0) / sec;
        const double mibps = (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / sec;

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "backend=random_4k_io\n";
        std::cout << "io_backend=" << args.backend << "\n";
        std::cout << "file=" << args.file << "\n";
        std::cout << "file_bytes=" << bytes << "\n";
        std::cout << "block_size=" << args.block_size << "\n";
        std::cout << "blocks=" << blocks << "\n";
        std::cout << "reads=" << total_reads << "\n";
        std::cout << "bytes=" << total_bytes << "\n";
        std::cout << "seed=" << args.seed << "\n";
        std::cout << "threads=" << args.threads << "\n";
        std::cout << "depth=" << args.depth << "\n";
        std::cout << "total_queue_depth=" << (args.backend == "uring" ? args.threads * args.depth : 0) << "\n";
        std::cout << "direct=" << (args.direct ? 1 : 0) << "\n";
        std::cout << "drop_page_cache=" << (args.drop_page_cache ? 1 : 0) << "\n";
        std::cout << "uring_registered_file=" << ((args.backend == "uring" && args.register_file) ? 1 : 0) << "\n";
        std::cout << "uring_sqpoll=" << ((args.backend == "uring" && args.uring_sqpoll) ? 1 : 0) << "\n";
        std::cout << "uring_iopoll=" << ((args.backend == "uring" && args.uring_iopoll) ? 1 : 0) << "\n";
        std::cout << "offset_prepare_ms=" << prep_ms << "\n";
        std::cout << "io_ms=" << io_ms << "\n";
        std::cout << "iops=" << iops << "\n";
        std::cout << "mbps=" << mbps << "\n";
        std::cout << "mibps=" << mibps << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
'''


def compile_runner(build_dir: Path, compiler: str) -> Path:
    build_dir.mkdir(parents=True, exist_ok=True)
    source = build_dir / "random_4k_io_runner.cpp"
    binary = build_dir / "random_4k_io_runner"
    if not source.exists() or source.read_text() != CPP_SOURCE:
        source.write_text(CPP_SOURCE)
    cmd = [
        compiler,
        "-O3",
        "-std=c++17",
        "-pthread",
        str(source),
        "-luring",
        "-o",
        str(binary),
    ]
    subprocess.check_call(cmd)
    return binary


def resolve_pages_bin(path: Path) -> Path:
    if path.is_dir():
        path = path / "pages.bin"
    if not path.exists():
        raise SystemExit(f"input does not exist: {path}")
    if not path.is_file():
        raise SystemExit(f"input is not a file: {path}")
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description="Measure pure random block reads against pages.bin")
    parser.add_argument("input", help="paged store directory or pages.bin")
    parser.add_argument("-n", "--num", type=int, default=100000, help="number of random reads")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--depth", type=int, default=128, help="io_uring queue depth per thread")
    parser.add_argument("--backend", choices=("uring", "pread"), default="uring")
    parser.add_argument("--block-size", type=int, default=4096)
    parser.add_argument("--drop-page-cache", action="store_true")
    parser.add_argument("--buffered", action="store_true", help="do not use O_DIRECT")
    parser.add_argument("--uring-sqpoll", action="store_true")
    parser.add_argument("--uring-iopoll", action="store_true")
    parser.add_argument("--no-register-file", action="store_true")
    parser.add_argument("--compiler", default="g++")
    args = parser.parse_args()

    pages_bin = resolve_pages_bin(Path(args.input))
    binary = compile_runner(Path("build"), args.compiler)
    cmd = [
        str(binary),
        str(pages_bin),
        "-n",
        str(args.num),
        "--seed",
        str(args.seed),
        "--threads",
        str(args.threads),
        "--depth",
        str(args.depth),
        "--backend",
        args.backend,
        "--block-size",
        str(args.block_size),
    ]
    if args.drop_page_cache:
        cmd.append("--drop-page-cache")
    if args.buffered:
        cmd.append("--buffered")
    if args.uring_sqpoll:
        cmd.append("--uring-sqpoll")
    if args.uring_iopoll:
        cmd.append("--uring-iopoll")
    if args.no_register_file:
        cmd.append("--no-register-file")
    subprocess.check_call(cmd)


if __name__ == "__main__":
    main()
