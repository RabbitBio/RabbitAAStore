#include "ProteinAAStore.hpp"
#include "CLI11.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <iomanip>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#include "omp.h"
// k-mer 最大支持 k=12 (5*12=60 bits < 64 bits)
constexpr int MAX_K = 12;
constexpr int BITS_PER_AA = 5;

// 获取系统可用内存 (MB)
uint64_t get_available_memory_mb() {
#ifdef __linux__
    // 方法1: 读取 /proc/meminfo 获取 MemAvailable（更准确）
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("MemAvailable:") == 0) {
                uint64_t mem_kb = 0;
                sscanf(line.c_str(), "MemAvailable: %lu kB", &mem_kb);
                return mem_kb / 1024;  // 转换为 MB
            }
        }
    }
    
    // 方法2: 使用 sysinfo（备用）
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        uint64_t free_mem = (si.freeram + si.bufferram) * si.mem_unit;
        return free_mem / (1024 * 1024);  // 转换为 MB
    }
#endif
    
    // 默认返回 4GB
    return 4096;
}

// 存储模式
enum class StorageMode {
    ARRAY,          // 数组模式：适合小 k 或高填充率
    HASHMAP,        // HashMap 模式：适合大 k 或稀疏数据
    MULTIPASS_ARRAY // 多轮数组模式：适合中等 k，内存受限
};

const char* storage_mode_name(StorageMode mode) {
    switch (mode) {
        case StorageMode::ARRAY: return "ARRAY (单次数组)";
        case StorageMode::HASHMAP: return "HASHMAP (哈希表)";
        case StorageMode::MULTIPASS_ARRAY: return "MULTIPASS_ARRAY (多轮数组)";
    }
    return "UNKNOWN";
}

// X/x 的 5-bit 编码值 ('X' & 0x1F = 24)
constexpr uint8_t AA_CODE_X = 24;

// 检查氨基酸编码是否有效（不是 X）
inline bool is_valid_aa(uint8_t aa_code) {
    return aa_code != AA_CODE_X;
}

// 检查序列片段是否包含 X
inline bool contains_x(const std::vector<uint8_t>& seq, size_t start, int k) {
    for (int i = 0; i < k; ++i) {
        if (seq[start + i] == AA_CODE_X) return true;
    }
    return false;
}

// 将 k-mer 编码为 uint64_t
inline uint64_t encode_kmer(const std::vector<uint8_t>& seq, size_t start, int k) {
    uint64_t kmer = 0;
    for (int i = 0; i < k; ++i) {
        kmer = (kmer << BITS_PER_AA) | seq[start + i];
    }
    return kmer;
}

// 滑动窗口编码（复用前一个 kmer）
inline uint64_t slide_kmer(uint64_t prev_kmer, uint8_t new_aa, uint64_t mask) {
    return ((prev_kmer << BITS_PER_AA) | new_aa) & mask;
}

// 将 uint64_t 解码为字符串
std::string decode_kmer(uint64_t kmer, int k) {
    std::string result(k, ' ');
    for (int i = k - 1; i >= 0; --i) {
        result[i] = ProteinAAStore::dec(kmer & 0x1F);
        kmer >>= BITS_PER_AA;
    }
    return result;
}

// 原子增加，带饱和（防止溢出）
inline void atomic_increment_saturating(std::atomic<uint32_t>& val) {
    uint32_t old_val = val.load(std::memory_order_relaxed);
    while (old_val < UINT32_MAX) {
        if (val.compare_exchange_weak(old_val, old_val + 1, 
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            break;
        }
    }
}

// 自适应 k-mer 计数器（多线程版本）
class AdaptiveKmerCounter {
public:
    int k_;
    uint64_t kmer_mask_;
    uint32_t min_count_;
    uint64_t max_memory_mb_;
    int num_threads_;
    
    // 分区参数（用于 MULTIPASS_ARRAY 模式）
    int prefix_bits_ = 0;
    int suffix_bits_ = 0;
    uint64_t num_partitions_ = 1;
    uint64_t partition_size_ = 0;
    
    // 存储模式
    StorageMode mode_ = StorageMode::ARRAY;
    
    // 统计结果
    std::vector<std::pair<uint64_t, uint64_t>> high_freq_kmers_;
    std::atomic<uint64_t> total_kmers_processed_{0};
    std::atomic<size_t> sequences_processed_{0};
    
    // 根据数据特征决定存储模式
    StorageMode decide_storage_mode(int k, uint64_t num_seqs, uint64_t total_length, 
                                    uint64_t max_memory_mb) {
        int total_bits = k * BITS_PER_AA;
        uint64_t kmer_space = 1ULL << total_bits;
        
        uint64_t array_memory_bytes = kmer_space * sizeof(std::atomic<uint32_t>);
        uint64_t max_memory_bytes = max_memory_mb * 1024ULL * 1024ULL;
        
        uint64_t expected_total_kmers = 0;
        if (total_length > num_seqs * (k - 1)) {
            expected_total_kmers = total_length - num_seqs * (k - 1);
        }
        
        uint64_t expected_unique_kmers = std::min(expected_total_kmers, kmer_space);
        double fill_rate = static_cast<double>(expected_unique_kmers) / kmer_space;
        
        constexpr uint64_t HASHMAP_ENTRY_SIZE = 24;
        uint64_t hashmap_memory_bytes = expected_unique_kmers * HASHMAP_ENTRY_SIZE;
        
        std::cerr << "存储模式决策参数:\n";
        std::cerr << "  k-mer 空间大小: " << kmer_space << " (32^" << k << ")\n";
        std::cerr << "  数组所需内存: " << (array_memory_bytes / 1024 / 1024) << " MB\n";
        std::cerr << "  预期 k-mer 总数: " << expected_total_kmers << "\n";
        std::cerr << "  预期不同 k-mer 数: " << expected_unique_kmers << "\n";
        std::cerr << "  预期填充率: " << std::fixed << std::setprecision(4) << (fill_rate * 100) << "%\n";
        std::cerr << "  HashMap 预估内存: " << (hashmap_memory_bytes / 1024 / 1024) << " MB\n";
        std::cerr << "  可用内存限制: " << max_memory_mb << " MB\n";
        
        if (array_memory_bytes <= max_memory_bytes && fill_rate > 0.01) {
            std::cerr << "  -> 选择 ARRAY 模式 (数组可装下且填充率高)\n";
            return StorageMode::ARRAY;
        }
        
        uint64_t estimated_passes = (array_memory_bytes + max_memory_bytes - 1) / max_memory_bytes;
        if (hashmap_memory_bytes <= max_memory_bytes * 0.8) {
            std::cerr << "  -> 选择 HASHMAP 模式 (HashMap 可装下，预计比 " 
                      << estimated_passes << " 轮数组更高效)\n";
            return StorageMode::HASHMAP;
        }
        
        if (fill_rate < 0.001 && estimated_passes > 8) {
            std::cerr << "  -> 选择 HASHMAP 模式 (数据极度稀疏，填充率 < 0.1%)\n";
            return StorageMode::HASHMAP;
        }
        
        std::cerr << "  -> 选择 MULTIPASS_ARRAY 模式 (需要 " << estimated_passes << " 轮)\n";
        return StorageMode::MULTIPASS_ARRAY;
    }
    
    void init(int k, uint64_t num_seqs, uint64_t total_length, 
              uint64_t max_memory_mb, uint32_t min_count, int num_threads) {
        k_ = k;
        min_count_ = min_count;
        max_memory_mb_ = max_memory_mb;
        num_threads_ = num_threads;
        
        int total_bits = k * BITS_PER_AA;
        kmer_mask_ = (1ULL << total_bits) - 1;
        
        mode_ = decide_storage_mode(k, num_seqs, total_length, max_memory_mb);
        
        if (mode_ == StorageMode::ARRAY) {
            prefix_bits_ = 0;
            suffix_bits_ = total_bits;
            num_partitions_ = 1;
            partition_size_ = 1ULL << suffix_bits_;
        } else if (mode_ == StorageMode::MULTIPASS_ARRAY) {
            uint64_t max_array_elements = (max_memory_mb * 1024ULL * 1024ULL) / sizeof(std::atomic<uint32_t>);
            
            suffix_bits_ = 0;
            while (suffix_bits_ < total_bits && (1ULL << (suffix_bits_ + 1)) <= max_array_elements) {
                suffix_bits_++;
            }
            suffix_bits_ = std::min(suffix_bits_, total_bits);
            prefix_bits_ = total_bits - suffix_bits_;
            
            num_partitions_ = (prefix_bits_ > 0) ? (1ULL << prefix_bits_) : 1;
            partition_size_ = 1ULL << suffix_bits_;
        }
        
        std::cerr << "\n存储模式: " << storage_mode_name(mode_) << "\n";
        std::cerr << "线程数: " << num_threads_ << "\n";
        if (mode_ != StorageMode::HASHMAP) {
            std::cerr << "分区策略: 总位数=" << total_bits 
                      << ", 前缀位数=" << prefix_bits_ 
                      << ", 后缀位数=" << suffix_bits_ << "\n";
            std::cerr << "分区数量: " << num_partitions_ 
                      << ", 每分区数组大小: " << partition_size_ 
                      << " (" << (partition_size_ * sizeof(std::atomic<uint32_t>) / 1024 / 1024) << " MB)\n";
        }
    }
    
    // ==================== ARRAY 模式（多线程）====================
    void run_array_mode(ProteinAAStore& store) {
        std::cerr << "开始 ARRAY 模式 k-mer 统计 (" << num_threads_ << " 线程)...\n";
        auto t_start = std::chrono::high_resolution_clock::now();
        
        // 分配原子计数数组
        std::vector<std::atomic<uint32_t>> counts(partition_size_);
        for (auto& c : counts) c.store(0, std::memory_order_relaxed);
        
        size_t total_seqs = store.size();
        sequences_processed_.store(0);
        total_kmers_processed_.store(0);
        
        // 工作线程函数
        auto worker = [&](size_t start_idx, size_t end_idx) {
            std::vector<uint8_t> seq_raw;
            uint64_t local_kmers = 0;
            
            for (size_t i = start_idx; i < end_idx; ++i) {
                size_t seq_len = store.get_raw(i, seq_raw);
                if (seq_len < static_cast<size_t>(k_)) continue;
                
                // 跟踪当前窗口中 X 的数量
                int x_count = 0;
                for (int p = 0; p < k_; ++p) {
                    if (seq_raw[p] == AA_CODE_X) ++x_count;
                }
                
                // 第一个 k-mer
                if (x_count == 0) {
                    uint64_t kmer = encode_kmer(seq_raw, 0, k_);
                    atomic_increment_saturating(counts[kmer]);
                    ++local_kmers;
                }
                
                // 滑动窗口
                uint64_t kmer = encode_kmer(seq_raw, 0, k_);
                for (size_t j = 1; j <= seq_len - k_; ++j) {
                    // 更新 X 计数：移出旧字符，移入新字符
                    if (seq_raw[j - 1] == AA_CODE_X) --x_count;
                    if (seq_raw[j + k_ - 1] == AA_CODE_X) ++x_count;
                    
                    kmer = slide_kmer(kmer, seq_raw[j + k_ - 1], kmer_mask_);
                    
                    if (x_count == 0) {
                        atomic_increment_saturating(counts[kmer]);
                        ++local_kmers;
                    }
                }
            }
            
            total_kmers_processed_.fetch_add(local_kmers, std::memory_order_relaxed);
            sequences_processed_.fetch_add(end_idx - start_idx, std::memory_order_relaxed);
        };
        
        // 启动进度报告线程
        std::atomic<bool> done{false};
        std::thread progress_thread([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!done.load()) {
                    std::cerr << "  进度: " << sequences_processed_.load() << "/" << total_seqs 
                              << " 序列, " << total_kmers_processed_.load() << " k-mers\n";
                }
            }
        });
        
        // 分配任务并启动线程
        std::vector<std::thread> threads;
        size_t chunk_size = (total_seqs + num_threads_ - 1) / num_threads_;
        
        for (int t = 0; t < num_threads_; ++t) {
            size_t start = t * chunk_size;
            size_t end = std::min(start + chunk_size, total_seqs);
            if (start < end) {
                threads.emplace_back(worker, start, end);
            }
        }
        
        for (auto& th : threads) th.join();
        
        done.store(true);
        progress_thread.join();
        
        // 提取高频 k-mer
        std::cerr << "提取高频 k-mer...\n";
        for (uint64_t i = 0; i < partition_size_; ++i) {
            uint32_t cnt = counts[i].load(std::memory_order_relaxed);
            if (cnt >= min_count_) {
                high_freq_kmers_.emplace_back(i, cnt);
            }
        }
        
        auto t_end = std::chrono::high_resolution_clock::now();
        std::cerr << "统计完成: 总共处理 " << total_kmers_processed_.load() << " 个 k-mer, "
                  << "保留 " << high_freq_kmers_.size() << " 个高频 k-mer\n";
        std::cerr << "统计耗时: " 
                  << std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count() 
                  << " 秒\n";
    }
    
    // ==================== HASHMAP 模式（多线程，每线程局部map后合并）====================
    void run_hashmap_mode(ProteinAAStore& store) {
        std::cerr << "开始 HASHMAP 模式 k-mer 统计 (" << num_threads_ << " 线程)...\n";
        auto t_start = std::chrono::high_resolution_clock::now();
        
        size_t total_seqs = store.size();
        sequences_processed_.store(0);
        total_kmers_processed_.store(0);
        
        // 每个线程的局部 hashmap
        std::vector<std::unordered_map<uint64_t, uint32_t>> thread_maps(num_threads_);
        
        // 工作线程函数
        auto worker = [&](int thread_id, size_t start_idx, size_t end_idx) {
            auto& local_map = thread_maps[thread_id];
            local_map.reserve((end_idx - start_idx) * 100);  // 预估每条序列 100 个 kmer
            
            std::vector<uint8_t> seq_raw;
            uint64_t local_kmers = 0;
            
            for (size_t i = start_idx; i < end_idx; ++i) {
                size_t seq_len = store.get_raw(i, seq_raw);
                if (seq_len < static_cast<size_t>(k_)) continue;
                
                // 跟踪当前窗口中 X 的数量
                int x_count = 0;
                for (int p = 0; p < k_; ++p) {
                    if (seq_raw[p] == AA_CODE_X) ++x_count;
                }
                
                uint64_t kmer = encode_kmer(seq_raw, 0, k_);
                if (x_count == 0) {
                    auto& cnt = local_map[kmer];
                    if (cnt < UINT32_MAX) ++cnt;
                    ++local_kmers;
                }
                
                for (size_t j = 1; j <= seq_len - k_; ++j) {
                    if (seq_raw[j - 1] == AA_CODE_X) --x_count;
                    if (seq_raw[j + k_ - 1] == AA_CODE_X) ++x_count;
                    
                    kmer = slide_kmer(kmer, seq_raw[j + k_ - 1], kmer_mask_);
                    
                    if (x_count == 0) {
                        auto& cnt2 = local_map[kmer];
                        if (cnt2 < UINT32_MAX) ++cnt2;
                        ++local_kmers;
                    }
                }
            }
            
            total_kmers_processed_.fetch_add(local_kmers, std::memory_order_relaxed);
            sequences_processed_.fetch_add(end_idx - start_idx, std::memory_order_relaxed);
        };
        
        // 启动进度报告线程
        std::atomic<bool> done{false};
        std::thread progress_thread([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!done.load()) {
                    std::cerr << "  进度: " << sequences_processed_.load() << "/" << total_seqs 
                              << " 序列, " << total_kmers_processed_.load() << " k-mers\n";
                }
            }
        });
        
        // 分配任务并启动线程
        std::vector<std::thread> threads;
        size_t chunk_size = (total_seqs + num_threads_ - 1) / num_threads_;
        
        for (int t = 0; t < num_threads_; ++t) {
            size_t start = t * chunk_size;
            size_t end = std::min(start + chunk_size, total_seqs);
            if (start < end) {
                threads.emplace_back(worker, t, start, end);
            }
        }
        
        for (auto& th : threads) th.join();
        
        done.store(true);
        progress_thread.join();
        
        // 合并所有线程的 hashmap
        std::cerr << "合并 " << num_threads_ << " 个线程的结果...\n";
        auto t_merge_start = std::chrono::high_resolution_clock::now();
        
        // 找到最大的 map 作为基础
        size_t max_idx = 0;
        size_t max_size = 0;
        for (int t = 0; t < num_threads_; ++t) {
            if (thread_maps[t].size() > max_size) {
                max_size = thread_maps[t].size();
                max_idx = t;
            }
        }
        
        auto& merged_map = thread_maps[max_idx];
        
        // 合并其他 map 到最大的 map
        for (int t = 0; t < num_threads_; ++t) {
            if (t == static_cast<int>(max_idx)) continue;
            for (auto& kv : thread_maps[t]) {
                auto& cnt = merged_map[kv.first];
                uint64_t new_cnt = static_cast<uint64_t>(cnt) + kv.second;
                cnt = (new_cnt > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(new_cnt);
            }
            thread_maps[t].clear();  // 释放内存
        }
        
        auto t_merge_end = std::chrono::high_resolution_clock::now();
        std::cerr << "合并完成, 不同 k-mer 数: " << merged_map.size() 
                  << ", 耗时: " << std::chrono::duration_cast<std::chrono::milliseconds>(t_merge_end - t_merge_start).count()
                  << " ms\n";
        
        // 提取高频 k-mer
        high_freq_kmers_.reserve(merged_map.size() / 10);
        for (const auto& kv : merged_map) {
            if (kv.second >= min_count_) {
                high_freq_kmers_.emplace_back(kv.first, kv.second);
            }
        }
        
        auto t_end = std::chrono::high_resolution_clock::now();
        std::cerr << "统计完成: 总共处理 " << total_kmers_processed_.load() << " 个 k-mer, "
                  << "保留 " << high_freq_kmers_.size() << " 个高频 k-mer\n";
        std::cerr << "统计耗时: " 
                  << std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count() 
                  << " 秒\n";
    }
    
    // ==================== MULTIPASS_ARRAY 模式（多线程）====================
    void process_partition_mt(ProteinAAStore& store, uint64_t partition_id,
                              std::vector<std::atomic<uint32_t>>& counts) {
        size_t total_seqs = store.size();
        sequences_processed_.store(0);
        std::atomic<uint64_t> partition_kmers{0};
        
        uint64_t prefix_shift = suffix_bits_;
        uint64_t target_prefix = partition_id;
        std::cerr << "Start OMP region" << std::endl;

        #pragma omp parallel num_threads(num_threads_)
        {
            std::vector<uint8_t> seq_raw;
            uint64_t local_kmers = 0;
            uint64_t local_seqs = 0;

            #pragma omp for schedule(guided)
            for (size_t i = 0; i < total_seqs; ++i) {
                ++local_seqs;
                size_t seq_len = store.get_raw(i, seq_raw);
                if (seq_len < static_cast<size_t>(k_)) continue;
                
                // 跟踪当前窗口中 X 的数量
                int x_count = 0;
                for (int p = 0; p < k_; ++p) {
                    if (seq_raw[p] == AA_CODE_X) ++x_count;
                }
                
                uint64_t kmer = encode_kmer(seq_raw, 0, k_);
                uint64_t prefix = kmer >> prefix_shift;
                uint64_t suffix = kmer & (partition_size_ - 1);
                
                if (x_count == 0 && prefix == target_prefix) {
                    atomic_increment_saturating(counts[suffix]);
                    ++local_kmers;
                }
                
                for (size_t j = 1; j <= seq_len - k_; ++j) {
                    if (seq_raw[j - 1] == AA_CODE_X) --x_count;
                    if (seq_raw[j + k_ - 1] == AA_CODE_X) ++x_count;
                    
                    kmer = slide_kmer(kmer, seq_raw[j + k_ - 1], kmer_mask_);
                    prefix = kmer >> prefix_shift;
                    suffix = kmer & (partition_size_ - 1);
                    
                    if (x_count == 0 && prefix == target_prefix) {
                        atomic_increment_saturating(counts[suffix]);
                        ++local_kmers;
                    }
                }
            }

            if (local_kmers > 0) {
                partition_kmers.fetch_add(local_kmers, std::memory_order_relaxed);
            }
            if (local_seqs > 0) {
                sequences_processed_.fetch_add(local_seqs, std::memory_order_relaxed);
            }
        }
        
        total_kmers_processed_.fetch_add(partition_kmers.load(), std::memory_order_relaxed);
        
        // 提取高频 k-mer
        uint64_t high_freq_count = 0;
        #pragma omp parallel num_threads(num_threads_) reduction(+:high_freq_count)
        {
            std::vector<std::pair<uint64_t, uint64_t>> local_kmers;
            uint64_t local_count = 0;

            #pragma omp for schedule(static)
            for (uint64_t i = 0; i < partition_size_; ++i) {
                uint32_t cnt = counts[i].load(std::memory_order_relaxed);
                if (cnt >= min_count_) {
                    uint64_t kmer = (partition_id << prefix_shift) | i;
                    local_kmers.emplace_back(kmer, cnt);
                    ++local_count;
                }
                counts[i].store(0, std::memory_order_relaxed);  // 重置供下一分区使用
            }

            if (!local_kmers.empty()) {
                #pragma omp critical
                {
                    high_freq_kmers_.insert(high_freq_kmers_.end(),
                                            local_kmers.begin(),
                                            local_kmers.end());
                }
            }
            high_freq_count += local_count;
        }
        
        std::cerr << "  分区 " << (partition_id + 1) << "/" << num_partitions_ 
                  << ": 处理 " << partition_kmers.load() << " 个 k-mer, "
                  << "保留 " << high_freq_count << " 个高频 k-mer\n";
    }
    
    void run_multipass_mode(ProteinAAStore& store) {
        std::cerr << "开始 MULTIPASS_ARRAY 模式 k-mer 统计 (共 " << num_partitions_ 
                  << " 轮, " << num_threads_ << " 线程)...\n";
        auto t_start = std::chrono::high_resolution_clock::now();
        auto t_init_begin = t_start;
        
        // 预分配原子计数数组（复用）
        auto t_counts_alloc_begin = std::chrono::high_resolution_clock::now();
        std::vector<std::atomic<uint32_t>> counts(partition_size_);
        auto t_counts_alloc_end = std::chrono::high_resolution_clock::now();
        auto t_counts_zero_begin = t_counts_alloc_end;
        #pragma omp parallel for num_threads(num_threads_) schedule(static)
        for (uint64_t i = 0; i < partition_size_; ++i) {
            counts[i].store(0, std::memory_order_relaxed);
        }
        auto t_counts_zero_end = std::chrono::high_resolution_clock::now();
        auto t_init_end = t_counts_zero_end;
        
        std::cerr << "初始化耗时统计:\n";
        std::cerr << "  counts 分配: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t_counts_alloc_end - t_counts_alloc_begin).count()
                  << " ms\n";
        std::cerr << "  counts 清零: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t_counts_zero_end - t_counts_zero_begin).count()
                  << " ms\n";
        std::cerr << "  初始化总计: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t_init_end - t_init_begin).count()
                  << " ms\n";

        std::cerr << "Start process_partition_mt" << std::endl;

        for (uint64_t p = 0; p < num_partitions_; ++p) {
            process_partition_mt(store, p, counts);
        }
        
        auto t_end = std::chrono::high_resolution_clock::now();
        std::cerr << "统计完成: 总共处理 " << total_kmers_processed_.load() << " 个 k-mer, "
                  << "保留 " << high_freq_kmers_.size() << " 个高频 k-mer\n";
        std::cerr << "统计耗时: " 
                  << std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count() 
                  << " 秒\n";
    }
    
    // ==================== 统一入口 ====================
    void run(ProteinAAStore& store) {
        switch (mode_) {
            case StorageMode::ARRAY:
                run_array_mode(store);
                break;
            case StorageMode::HASHMAP:
                run_hashmap_mode(store);
                break;
            case StorageMode::MULTIPASS_ARRAY:
                run_multipass_mode(store);
                break;
        }
    }
};

// 频次分布分析器
class FrequencyAnalyzer {
public:
    static std::vector<std::pair<uint64_t, uint64_t>> build_histogram(
        const std::vector<std::pair<uint64_t, uint64_t>>& kmers) {
        
        if (kmers.empty()) return {};
        
        std::vector<uint64_t> freq_counts;
        for (const auto& kv : kmers) {
            uint64_t freq = kv.second;
            if (freq >= freq_counts.size()) {
                freq_counts.resize(freq + 1, 0);
            }
            freq_counts[freq]++;
        }
        
        std::vector<std::pair<uint64_t, uint64_t>> histogram;
        for (uint64_t i = 0; i < freq_counts.size(); ++i) {
            if (freq_counts[i] > 0) {
                histogram.emplace_back(i, freq_counts[i]);
            }
        }
        return histogram;
    }
    
    static uint64_t find_frequency_cutoff(
        const std::vector<std::pair<uint64_t, uint64_t>>& histogram,
        double percentile = 0.95) {
        
        if (histogram.size() < 3) {
            return histogram.empty() ? 0 : histogram.back().first;
        }
        
        uint64_t total = 0;
        for (const auto& h : histogram) {
            total += h.second;
        }
        
        uint64_t cumsum = 0;
        uint64_t percentile_cutoff = histogram.back().first;
        for (const auto& h : histogram) {
            cumsum += h.second;
            if (static_cast<double>(cumsum) / total >= percentile) {
                percentile_cutoff = h.first;
                break;
            }
        }
        
        std::vector<double> log_freq, log_count;
        for (const auto& h : histogram) {
            if (h.first > 0 && h.second > 0) {
                log_freq.push_back(std::log(static_cast<double>(h.first)));
                log_count.push_back(std::log(static_cast<double>(h.second)));
            }
        }
        
        if (log_freq.size() < 5) {
            return percentile_cutoff;
        }
        
        double max_curvature = 0;
        size_t elbow_idx = 0;
        for (size_t i = 2; i < log_freq.size() - 2; ++i) {
            double d2 = (log_count[i-2] - 2*log_count[i] + log_count[i+2]) / 
                        std::pow(log_freq[i+2] - log_freq[i-2], 2);
            double curvature = std::abs(d2);
            if (curvature > max_curvature) {
                max_curvature = curvature;
                elbow_idx = i;
            }
        }
        
        uint64_t elbow_cutoff = histogram[0].first;
        size_t hist_idx = 0;
        for (size_t i = 0; i < histogram.size() && hist_idx <= elbow_idx; ++i) {
            if (histogram[i].first > 0 && histogram[i].second > 0) {
                if (hist_idx == elbow_idx) {
                    elbow_cutoff = histogram[i].first;
                    break;
                }
                hist_idx++;
            }
        }
        
        return std::max(percentile_cutoff, elbow_cutoff);
    }
    
    static void print_summary(std::ostream& out,
                              const std::vector<std::pair<uint64_t, uint64_t>>& histogram,
                              uint64_t cutoff) {
        if (histogram.empty()) {
            out << "# 无频次数据\n";
            return;
        }
        
        uint64_t total_kmers = 0;
        uint64_t total_occurrences = 0;
        uint64_t min_freq = histogram.front().first;
        uint64_t max_freq = histogram.back().first;
        
        for (const auto& h : histogram) {
            total_kmers += h.second;
            total_occurrences += h.first * h.second;
        }
        
        uint64_t high_freq_kmers = 0;
        uint64_t high_freq_occurrences = 0;
        for (const auto& h : histogram) {
            if (h.first >= cutoff) {
                high_freq_kmers += h.second;
                high_freq_occurrences += h.first * h.second;
            }
        }
        
        out << "# ========== 频次分布摘要 ==========\n";
        out << "# 总 k-mer 种类数: " << total_kmers << "\n";
        out << "# 总 k-mer 出现次数: " << total_occurrences << "\n";
        out << "# 频次范围: [" << min_freq << ", " << max_freq << "]\n";
        out << "# 自动检测的频次阈值 (elbow): " << cutoff << "\n";
        out << "# 高频 k-mer (>=" << cutoff << "): " << high_freq_kmers 
            << " 种 (" << std::fixed << std::setprecision(2)
            << (100.0 * high_freq_kmers / total_kmers) << "%)\n";
        out << "# 高频 k-mer 出现次数占比: " << std::fixed << std::setprecision(2)
            << (100.0 * high_freq_occurrences / total_occurrences) << "%\n";
        out << "# ==================================\n";
    }
    
    static void print_histogram(std::ostream& out,
                                const std::vector<std::pair<uint64_t, uint64_t>>& histogram,
                                int max_bins = 50) {
        if (histogram.empty()) return;
        
        out << "# 频次分布直方图 (freq -> count):\n";
        
        if (histogram.size() <= static_cast<size_t>(max_bins)) {
            for (const auto& h : histogram) {
                out << "# " << std::setw(10) << h.first << " -> " << h.second << "\n";
            }
        } else {
            uint64_t min_freq = histogram.front().first;
            uint64_t max_freq = histogram.back().first;
            double log_min = std::log10(std::max(1.0, static_cast<double>(min_freq)));
            double log_max = std::log10(static_cast<double>(max_freq));
            double bin_width = (log_max - log_min) / max_bins;
            
            std::vector<uint64_t> bin_counts(max_bins, 0);
            std::vector<uint64_t> bin_lower(max_bins);
            
            for (int i = 0; i < max_bins; ++i) {
                bin_lower[i] = static_cast<uint64_t>(std::pow(10, log_min + i * bin_width));
            }
            
            for (const auto& h : histogram) {
                int bin = static_cast<int>((std::log10(std::max(1.0, static_cast<double>(h.first))) - log_min) / bin_width);
                bin = std::max(0, std::min(max_bins - 1, bin));
                bin_counts[bin] += h.second;
            }
            
            for (int i = 0; i < max_bins; ++i) {
                if (bin_counts[i] > 0) {
                    uint64_t upper = (i + 1 < max_bins) ? bin_lower[i + 1] - 1 : max_freq;
                    out << "# [" << std::setw(8) << bin_lower[i] << " - " 
                        << std::setw(8) << upper << "] -> " << bin_counts[i] << "\n";
                }
            }
        }
    }
};

int main(int argc, char* argv[]) {
    CLI::App app{"K-mer 频次统计工具 (多线程版本)"};
    
    int k = 5;
    std::string input_file;
    std::string output_file;
    uint64_t max_memory_mb = 0;  // 0 表示自动检测可用内存
    uint32_t min_count = 0;
    int top_n = 0;
    bool output_histogram = true;   // 默认开启直方图输出
    double percentile = 0.95;
    uint64_t freq_cutoff = 0;
    std::string force_mode;
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    
    app.add_option("-k,--kmer", k, "k-mer 长度 (1-12)")
       ->required()
       ->check(CLI::Range(1, MAX_K));
    
    app.add_option("-i,--input", input_file, "输入的 ProteinAAStore 二进制文件")
       ->required()
       ->check(CLI::ExistingFile);
    
    app.add_option("-o,--output", output_file, "输出文件 (默认: 标准输出)");
    
    app.add_option("-M,--memory", max_memory_mb, 
                   "最大内存限制 (MB, 默认: 0=自动检测可用内存的80%)");
    
    app.add_option("-m,--min-count", min_count, 
                   "最小保留频次 (默认: 0=根据序列数自动计算)");
    
    app.add_option("-t,--top", top_n, 
                   "只输出频次最高的前 N 个 k-mer (默认: 0=全部)");
    
    app.add_option("-c,--cutoff", freq_cutoff,
                   "高频 k-mer 的频次阈值 (默认: 0=自动检测)");
    
    app.add_option("-p,--percentile", percentile,
                   "自动检测阈值时使用的分位数 (默认: 0.95)")
       ->check(CLI::Range(0.5, 0.9999));
    
    app.add_flag("-H,--histogram,!--no-histogram", output_histogram, 
                 "输出频次分布直方图 (默认: 开启, 使用 --no-histogram 关闭)");
    
    app.add_option("--mode", force_mode,
                   "强制指定存储模式: array, hashmap, multipass (默认: 自动选择)");
    
    app.add_option("-j,--threads", num_threads,
                   "线程数 (默认: CPU 核心数)")
       ->check(CLI::Range(1, 256));
    
    CLI11_PARSE(app, argc, argv);
    
    // 加载数据
    std::cerr << "正在加载 " << input_file << " ...\n";
    auto t_start = std::chrono::high_resolution_clock::now();
    
    ProteinAAStore store;
    if (!store.load(input_file)) {
        std::cerr << "错误: 无法加载文件 " << input_file << "\n";
        return 1;
    }
    
    auto t_load = std::chrono::high_resolution_clock::now();
    std::cerr << "加载完成: " << store.size() << " 条序列, 总长度 " 
              << store.total_length() << " 氨基酸\n";
    
    // 自动计算可用内存
    if (max_memory_mb == 0) {
        uint64_t available_mb = get_available_memory_mb();
        // 已加载的数据占用的内存（仅用于显示，数据已加载所以 available_mb 已经扣除了这部分）
        uint64_t store_memory_mb = store.memory_usage() / (1024 * 1024);
        
        // available_mb 是数据加载后的剩余可用内存
        // 预留 10% 作为系统余量 + 2GB 用于程序其他开销（排序、输出缓冲等）
        uint64_t reserved_mb = available_mb / 10 + 2048;
        
        if (available_mb > reserved_mb + 1024) {  // 至少要有 1GB 可用
            max_memory_mb = available_mb - reserved_mb;
        } else {
            // 可用内存太少，尝试用可用内存的 80%
            max_memory_mb = (available_mb * 80) / 100;
        }
        
        // 确保在合理范围内
        max_memory_mb = std::max(max_memory_mb, (uint64_t)1024);  // 最小 1GB
        max_memory_mb = std::min(max_memory_mb, (uint64_t)1048576);  // 最大 1TB
        
        std::cerr << "系统当前可用内存: " << available_mb << " MB\n";
        std::cerr << "数据已占用内存: " << store_memory_mb << " MB\n";
        std::cerr << "自动设置工作内存: " << max_memory_mb << " MB (当前可用的 ~" 
                  << (100 * max_memory_mb / available_mb) << "%)\n";
    }
    
    // 自动计算 min_count
    if (min_count == 0) {
        if (store.size() <= 1000000) {
            min_count = 2;
        } else {
            min_count = 2 + static_cast<uint32_t>(store.size() / 1000000);
        }
        std::cerr << "自动设置最小保留频次 min_count = " << min_count 
                  << " (基于 " << store.size() << " 条序列)\n";
    }

    std::cerr << "加载耗时: " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(t_load - t_start).count() 
              << " ms\n";
    
    // 初始化计数器
    AdaptiveKmerCounter counter;
    counter.init(k, store.size(), store.total_length(), max_memory_mb, min_count, num_threads);
    
    // 如果用户强制指定了模式，覆盖自动决策
    if (!force_mode.empty()) {
        if (force_mode == "array") {
            counter.mode_ = StorageMode::ARRAY;
            int total_bits = k * BITS_PER_AA;
            counter.prefix_bits_ = 0;
            counter.suffix_bits_ = total_bits;
            counter.num_partitions_ = 1;
            counter.partition_size_ = 1ULL << total_bits;
        } else if (force_mode == "hashmap") {
            counter.mode_ = StorageMode::HASHMAP;
        } else if (force_mode == "multipass") {
            counter.mode_ = StorageMode::MULTIPASS_ARRAY;
        } else {
            std::cerr << "警告: 未知的模式 '" << force_mode << "', 使用自动选择\n";
        }
        std::cerr << "强制使用存储模式: " << storage_mode_name(counter.mode_) << "\n";
    }
    
    // 运行统计
    counter.run(store);
    
    // 按频次降序排序
    std::cerr << "正在排序结果...\n";
    std::sort(counter.high_freq_kmers_.begin(), counter.high_freq_kmers_.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // 构建频次分布直方图
    auto histogram = FrequencyAnalyzer::build_histogram(counter.high_freq_kmers_);
    
    // 自动检测频次阈值
    uint64_t detected_cutoff = freq_cutoff;
    if (detected_cutoff == 0 && !histogram.empty()) {
        detected_cutoff = FrequencyAnalyzer::find_frequency_cutoff(histogram, percentile);
    }
    
    // 准备输出
    std::ostream* out = &std::cout;
    std::ofstream out_file;
    if (!output_file.empty()) {
        out_file.open(output_file);
        if (!out_file) {
            std::cerr << "错误: 无法创建输出文件 " << output_file << "\n";
            return 1;
        }
        out = &out_file;
    }
    
    // 输出频次分布摘要
    FrequencyAnalyzer::print_summary(*out, histogram, detected_cutoff);
    
    // 输出直方图
    if (output_histogram) {
        FrequencyAnalyzer::print_histogram(*out, histogram);
    }
    
    // 输出高频 k-mer 列表
    *out << "# ========== 高频 k-mer 列表 ==========\n";
    *out << "# kmer\tcount\n";
    
    size_t output_count = 0;
    for (const auto& kv : counter.high_freq_kmers_) {
        if (freq_cutoff > 0 && kv.second < freq_cutoff) continue;
        
        *out << decode_kmer(kv.first, k) << "\t" << kv.second << "\n";
        ++output_count;
        
        if (top_n > 0 && output_count >= static_cast<size_t>(top_n)) break;
    }
    
    if (!output_file.empty()) {
        std::cerr << "结果已写入 " << output_file << " (" << output_count << " 行)\n";
    }
    
    auto t_end = std::chrono::high_resolution_clock::now();
    std::cerr << "总耗时: "
              << std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count()
              << " 秒\n";
    
    return 0;
}
