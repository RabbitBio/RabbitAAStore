# AAStore 批量取数与预取优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标：** 减少 full-memory 输出准备开销，消除重复跨页请求造成的重复 page I/O，并把连续 miss page 合并为最多 16 页的异步读取。

**架构：** `FetchResult` 改用 no-init arena 并通过 offsets 直接寻址；批请求仍按 seq_id 排序，但相同 ID 按 page-major 展开；cache miss 分类后生成最多 64 KiB 的合并 `PrefetchRange`。所有模式统一使用 `Options::io_threads` 作为批处理线程数。

**技术栈：** C++17、PagedProteinAAStore、CLI11、pthread、可选 liburing、CMake。

## 全局约束

- 不改变磁盘格式、5-bit 编码、序列结果顺序和 cache 容量语义。
- 不读取未请求页面，`prefetch_read_bytes` 不得因合并增加。
- 一个合并 range 最多 16 个完整 4 KiB 页面。
- 文件末尾短页不进入 O_DIRECT 合并 range。
- 保留 `FetchResult::output_ptrs` 字段用于源码兼容，但 fetch 完成后必须为空。
- 每项行为修改都先运行失败测试，再写实现。

---

### 任务 1：建立确定性批量读取回归测试

**文件：**
- 新建：`src/paged_batch_regression.cpp`
- 修改：`CMakeLists.txt`

**接口：**
- 使用：`PagedProteinAAStore::build_from_sequence_views()`、`fetch_into()`、`batch_get_into()`、`stats()`。
- 产出：可执行程序 `paged_batch_regression`，失败返回非零，成功打印 `paged batch regression: ok`。

- [ ] **步骤 1：写测试 store 和结果校验工具**

在临时目录构建 page_residues 为 64 的小库，序列包含空序列、单页序列、跨 4 页序列和跨 17 页序列。测试结束使用 `std::filesystem::remove_all()` 清理。

- [ ] **步骤 2：写当前实现必然失败的断言**

```cpp
expect(fetch.output_ptrs.empty(), "fetch should not retain output pointers");
expect(stats.full_cache_decode_threads == 4,
       "full cache should use unified worker count");
expect(repeated_stats.prefetch_miss_pages == physical_pages,
       "repeated multi-page IDs should not revisit pages");
expect(four_page_stats.prefetch_ranges == 1,
       "four adjacent pages should coalesce");
expect(seventeen_page_stats.prefetch_ranges == 2,
       "seventeen adjacent pages should form two ranges");
```

同时逐条比较 `fetch().view(i)` 与 `get(id)`，并检查 `storage[offsets[i + 1] - 1] == '\0'`。

- [ ] **步骤 3：把测试目标接入 CMake**

新增 `paged_batch_regression` target，链接 `Threads::Threads`；检测到 liburing 时使用与 `paged_aatools` 相同的宏、include 和 library。

- [ ] **步骤 4：运行测试，确认按预期失败**

```bash
cmake --build build -j4 --target paged_batch_regression
./build/paged_batch_regression
```

预期：在 output pointers、统一线程、重复 page 或 range 合并断言处失败，而不是编译错误或序列内容错误。

- [ ] **步骤 5：提交测试**

```bash
git add CMakeLists.txt src/paged_batch_regression.cpp
git commit -m "test: cover paged batch fetch and range coalescing"
```

### 任务 2：优化 FetchResult 并统一线程数

**文件：**
- 修改：`src/PagedProteinAAStore.hpp:149-225`
- 修改：`src/PagedProteinAAStore.hpp:3338-3584`
- 修改：`src/PagedAAStoreTools.cpp:714-780`
- 测试：`src/paged_batch_regression.cpp`

**接口：**
- 产出：`FetchResult::storage` 使用 no-init allocator；`output_ptrs` 为空；`Options::io_threads` 控制所有 batch worker。

- [ ] **步骤 1：让 no-init storage 测试在编译期表达要求**

```cpp
static_assert(std::is_same_v<
    typename decltype(PagedProteinAAStore::FetchResult{}.storage)::allocator_type,
    AANoInitAllocator<char>>);
```

重新构建，预期 static_assert 失败。

- [ ] **步骤 2：修改 FetchResult 和 arena 输出路径**

定义 `using FetchStorage = std::vector<char, AANoInitAllocator<char>>;`，将 `storage` 改为该类型。`prepare_fetch_result()` 只准备 offsets、requests 和 storage，不再 resize/fill `output_ptrs`。

`fetch_into_prefetch_impl()` 使用：

```cpp
ArenaOutputSource output_source{out.storage.data(), out.offsets.data()};
```

传给 resident decode 和 request decode。HotSeq 分支直接接收 prepared requests 与 arena output，不重新建立 ids/outputs。

- [ ] **步骤 3：在最后 chunk 写终止符**

给 `BatchChunk` 增加 `bool terminates_output`。构建 chunk 时标记每个请求的最后一段。`decode_group()` 解码后调用 output-source 特化：arena 写 `\0`，普通 `char**`/SequenceRecord 不操作。空序列在 arena 准备阶段写 `\0`。

删除 fetch 路径对 `terminate_fetch_result()` 的调用；C API arena 结果保持相同终止符语义。

- [ ] **步骤 4：统一线程参数**

`configure()` 同时执行：

```cpp
set_io_threads(options.io_threads);
set_full_cache_decode_threads(options.io_threads);
```

full raw cache 路径直接使用 `full_cache_decode_threads_`。CLI 把 `--threads,--prefetch-threads,--decode-threads` 绑定到同一个变量，并删除独立 decode_threads 赋值。

- [ ] **步骤 5：运行回归测试**

预期：输出、终止符、no-init allocator 和统一线程断言通过；page 重复与 range 合并断言仍失败。

- [ ] **步骤 6：提交输出与线程修改**

```bash
git add src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp src/paged_batch_regression.cpp
git commit -m "perf: streamline fetch arena and unify batch threads"
```

### 任务 3：按 page 优先展开重复 seq_id

**文件：**
- 修改：`src/PagedProteinAAStore.hpp:4104-4163`
- 测试：`src/paged_batch_regression.cpp`

**接口：**
- 保持 `SequenceRequest` 和 `BatchChunk` 对外不可见。
- 保证生成的 `chunks[].page_id` 非递减。

- [ ] **步骤 1：确认重复跨页测试失败原因**

运行测试并记录当前 `prefetch_miss_pages` 大于物理页面数，确认失败来自 `A,B,A,B` page group 重访。

- [ ] **步骤 2：按相同 seq_id run 构建 chunks**

```cpp
for (size_t run_begin = 0; run_begin < ordered_requests.size();) {
    size_t run_end = run_begin + 1;
    while (run_end < ordered_requests.size() &&
           ordered_requests[run_end].seq_id == ordered_requests[run_begin].seq_id) {
        ++run_end;
    }
    // 每个 page piece 外层循环，每个重复 request 内层循环。
    run_begin = run_end;
}
```

同一 run 只计算一次 start/length/page 分段；每个 page piece 为 run 中每个 request 生成一个 chunk。逻辑统计按 run 长度累计。

- [ ] **步骤 3：增加单调性保护**

构建 group 前验证 `chunks[i - 1].page_id <= chunks[i].page_id`，违反时抛出 `corrupt sequence metadata`。

- [ ] **步骤 4：运行回归测试**

预期：重复跨页 page miss 测试通过；range 合并测试仍失败；所有序列结果一致。

- [ ] **步骤 5：提交重复页修复**

```bash
git add src/PagedProteinAAStore.hpp src/paged_batch_regression.cpp
git commit -m "perf: group repeated sequence chunks by page"
```

### 任务 4：合并最多 16 个相邻 miss 页面

**文件：**
- 修改：`src/PagedProteinAAStore.hpp:3192-3242`
- 修改：`src/PagedProteinAAStore.hpp:4379-6393`
- 测试：`src/paged_batch_regression.cpp`

**接口：**
- `CacheStats::prefetch_miss_pages` 保持物理 miss page 数量。
- `CacheStats::prefetch_ranges` 表示实际 I/O 请求数量。
- 每个 range 最多 16 个页面。

- [ ] **步骤 1：扩展 PrefetchRange**

```cpp
size_t first_group_index;
uint16_t group_count;
uint16_t admission_mask;
uint64_t first_page_id;
uint64_t file_offset;
uint64_t bytes_to_read;
bool buffered_fallback;
```

增加 `static constexpr size_t max_prefetch_pages_per_range = 16;`。

- [ ] **步骤 2：在 range builder 中只合并连续 miss**

只有下一个 group index、page ID 和文件偏移都连续，且当前 range 少于 16 个完整页时才追加。每页独立执行 page-hot 与 2Q-lite admission 判断并设置 admission mask。

```cpp
stats_.prefetch_miss_pages += miss_group_indices.size();
stats_.prefetch_ranges += ranges.size();
stats_.prefetch_read_bytes += sum(range.bytes_to_read);
```

- [ ] **步骤 3：把 prefetch slot 改为 64 KiB stride**

`fixed_block_bytes` 使用 `packed_page_bytes_ * 16`。pool slot 数由 `prefetch_memory_bytes / fixed_block_bytes` 计算，至少保留一个 slot。

- [ ] **步骤 4：一次读取 range 并逐页解码**

pread/AIO/io_uring worker 以 range 的 `bytes_to_read` 提交一次读取。完成后循环 `group_count`，从 `buffer + page_offset * packed_page_bytes_` 构造 PageView，调用 `decode_group()`，并按 admission mask 插入 LRU。

miss 和 admission 按页面计数，I/O range 按实际提交次数计数。

- [ ] **步骤 5：处理短尾页 fallback**

range 长度不是 4096 对齐时标记 `buffered_fallback=true` 且不合并。异步 O_DIRECT worker 使用 `pages_fd_` buffered pread，完成后进入相同 ready/decode 流程。

- [ ] **步骤 6：运行完整回归测试**

预期：4 页为 1 range、17 页为 2 ranges；prefix/LRU/no-memory/full-memory 结果一致；ASan/UBSan 无错误。

- [ ] **步骤 7：提交 range 合并**

```bash
git add src/PagedProteinAAStore.hpp src/paged_batch_regression.cpp
git commit -m "perf: coalesce adjacent missing pages"
```

### 任务 5：性能与兼容性验证

**文件：**
- 修改：仅在发现回归时修改上述实现文件。
- 输出：当前目录保留有效结果，删除 `/tmp` 临时 store 和二进制。

- [ ] **步骤 1：运行普通和 sanitizer 回归**

```bash
cmake --build build -j4 --target paged_batch_regression paged_aatools
./build/paged_batch_regression
g++ -O1 -g -fsanitize=address,undefined -std=c++17 -pthread \
    -Isrc src/paged_batch_regression.cpp -o /tmp/paged_batch_regression_asan
/tmp/paged_batch_regression_asan
```

- [ ] **步骤 2：运行 1M 性能对比**

修改前基线：full-memory `output_prepare_ms≈132ms`，32 线程 `store_get_ms≈11.7ms`；no-memory 重复请求 `prefetch_ranges=94942`，库总页数 `54953`。

确认修改后 `output_ptrs` 为空、实际线程等于 `--threads`、page miss 不超过 distinct page 数、`prefetch_ranges < prefetch_miss_pages`、读取字节不增加且 checksum 不变。

- [ ] **步骤 3：运行 10M 冷启动对比**

分别测 no-memory 和 full-memory 的 wall、open、preload、output prepare、store get、I/O wall、decode、range 数和 RSS。运行前检查没有其他大任务占用 CPU/I/O。

- [ ] **步骤 4：清理临时文件并检查 diff**

```bash
rm -rf /tmp/aastore_batch_regression_* /tmp/paged_batch_regression_asan
git diff --check
git status --short
```

- [ ] **步骤 5：最终提交**

只提交本计划涉及的文件，不包含用户已有的其他修改或构建产物。
