# Page-hot LRU Default Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将半内存 resident cache 收敛为 `prefix` 与 `page_hot_lru` 两种策略，删除普通 LRU 和 admission freeze，同时保留旧 LRU 名称作为兼容别名。

**Architecture:** `ResidentCachePolicy` 只保留 canonical 的 `PageHotLru` 与 `Prefix`。`PageHotLru` 始终先按单批 page 引用数过滤，再进入现有 2Q-lite 二次访问 admission；旧字符串只在解析边界映射到 canonical 策略。删除 freeze 配置、状态和统计，让页热度阈值成为唯一批内 admission 门槛。

**Tech Stack:** C++17、CLI11、header-only `PagedProteinAAStore`、现有 `paged_batch_regression`、Python ctypes wrapper、io_uring benchmark。

## Global Constraints

- canonical 策略只允许 `prefix` 与 `page_hot_lru`。
- `lru`、`lru_2q_lite`、`2q_lite` 必须兼容解析为 `page_hot_lru`。
- 默认 `page_hot_lru_min_refs` 固定为 32，但 CLI 仍可覆盖。
- 删除普通 LRU、freeze 行为及其公开统计。
- 不改变压缩格式、meta 格式、page 布局和序列返回结果。
- 不修改无关的 dirty worktree 文件。

---

### Task 1: Canonical 策略与接口收敛

**Files:**
- Modify: `src/PagedProteinAAStore.hpp:133-183,2283-2345,7057-7070,7628-7643`
- Modify: `src/PagedAAStoreTools.cpp:748-818,1338-1360,1595-1610`
- Modify: `python/aastore_backend.py:300-318`
- Test: `src/paged_batch_regression.cpp`

**Interfaces:**
- Produces: `ResidentCachePolicy::PageHotLru`
- Produces: `parse_resident_cache_policy("page_hot_lru") -> PageHotLru`
- Produces: `resident_cache_policy_name(PageHotLru) -> "page_hot_lru"`
- Produces: `Options::page_hot_lru_min_refs`, default 32

- [ ] **Step 1: 写失败的策略接口测试**

在 `paged_batch_regression.cpp` 的 `main()` 开头加入：

```cpp
{
    using Policy = PagedProteinAAStore::ResidentCachePolicy;
    PagedProteinAAStore::Options defaults;
    expect(defaults.cache_policy == Policy::PageHotLru,
           "default resident policy should be page_hot_lru");
    expect(defaults.page_hot_lru_min_refs == 32,
           "default page-hot threshold should be 32");
    for (const std::string& alias : {
             "page_hot_lru", "lru", "lru_2q_lite", "2q_lite"}) {
        Policy parsed = PagedProteinAAStore::parse_resident_cache_policy(alias);
        expect(parsed == Policy::PageHotLru,
               "LRU compatibility alias should parse as page_hot_lru");
        expect(std::string(PagedProteinAAStore::resident_cache_policy_name(parsed)) ==
                   "page_hot_lru",
               "LRU compatibility alias should report canonical name");
    }
}
```

- [ ] **Step 2: 运行测试并确认失败**

Run:

```bash
cmake --build /tmp/aastore-halfmem-build --target paged_batch_regression -j4
```

Expected: 编译失败，提示 `PageHotLru` 不存在。

- [ ] **Step 3: 实现 canonical enum、解析和默认值**

在 `PagedProteinAAStore.hpp` 中：

```cpp
enum class ResidentCachePolicy : uint8_t {
    PageHotLru = 0,
    Prefix = 1
};

// Options
ResidentCachePolicy cache_policy = ResidentCachePolicy::PageHotLru;
uint32_t page_hot_lru_min_refs = 32;
```

删除 `Options::page_hot_lru`。更新名称和解析：

```cpp
static const char* resident_cache_policy_name(ResidentCachePolicy policy) {
    switch (policy) {
        case ResidentCachePolicy::PageHotLru: return "page_hot_lru";
        case ResidentCachePolicy::Prefix: return "prefix";
    }
    return "unknown";
}

static ResidentCachePolicy parse_resident_cache_policy(const std::string& policy) {
    if (policy == "page_hot_lru" || policy == "lru" ||
        policy == "lru_2q_lite" || policy == "2q_lite") {
        return ResidentCachePolicy::PageHotLru;
    }
    if (policy == "prefix" || policy == "non-lru" || policy == "non_lru") {
        return ResidentCachePolicy::Prefix;
    }
    throw std::invalid_argument("unknown resident cache policy: " + policy);
}
```

将所有 `ResidentCachePolicy::Lru` 引用机械替换为 `PageHotLru`。将 `set_page_hot_lru(bool, uint32_t)` 收敛为：

```cpp
void set_page_hot_lru_min_refs(uint32_t min_refs) {
    page_hot_lru_min_refs_ = std::max<uint32_t>(1, min_refs);
}
```

`configure()` 只调用该阈值 setter。同步删除内部 `page_hot_lru_` bool，并让 canonical policy 固定建立 page-hot plan：

```cpp
LruPageHotPlan build_lru_page_hot_plan(bool allow_lru_admission) const {
    LruPageHotPlan plan;
    if (!allow_lru_admission) return plan;
    plan.enabled = true;
    plan.min_refs = std::min<uint32_t>(
        page_hot_ref_cap(), std::max<uint32_t>(1, page_hot_lru_min_refs_));
    return plan;
}
```

- [ ] **Step 4: 收敛 CLI 和 Python 默认接口**

在 `PagedAAStoreTools.cpp`：

```cpp
std::string cache_policy = "page_hot_lru";
```

删除 `bool page_hot_lru`、`--page-hot-lru` flag、`store_options.page_hot_lru` 赋值和 `page_hot_lru=` 输出。`--cache-policy` 的成员集合改为：

```cpp
{"page_hot_lru", "lru", "lru_2q_lite", "2q_lite",
 "prefix", "non-lru", "non_lru"}
```

帮助文本明确只公开 `page_hot_lru` 与 `prefix`。在 `python/aastore_backend.py` 中将默认值改为：

```python
cache_policy = kwargs.pop(
    "cache_policy", os.environ.get("AASTORE_CACHE_POLICY", "page_hot_lru")
)
```

- [ ] **Step 5: 运行策略接口回归**

Run:

```bash
cmake --build /tmp/aastore-halfmem-build --target paged_batch_regression paged_aatools -j4
/tmp/aastore-halfmem-build/paged_batch_regression
/tmp/aastore-halfmem-build/paged_aatools bench --help
```

Expected: regression 输出 `paged batch regression: ok`；帮助中出现 `page_hot_lru` 与 `prefix`，不再出现独立 `--page-hot-lru` flag。

- [ ] **Step 6: 提交策略接口改动**

```bash
git add src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp \
        src/paged_batch_regression.cpp python/aastore_backend.py
git commit -m "Make page-hot LRU the canonical resident policy"
```

---

### Task 2: 删除 freeze 并锁定 page-hot 2Q 行为

**Files:**
- Modify: `src/PagedProteinAAStore.hpp:350-410,4790-4888,7057-7070,7628-7698,9190-9210`
- Modify: `src/PagedAAStoreTools.cpp:1659-1672`
- Test: `src/paged_batch_regression.cpp`

**Interfaces:**
- Consumes: `ResidentCachePolicy::PageHotLru`
- Produces: `lru_page_hot_candidate_pages/filtered_pages`
- Preserves: 2Q-lite first miss and promotion counters

- [ ] **Step 1: 写低密度过滤与高密度二次 admission 测试**

扩展 `open_half_memory()`，允许传阈值：

```cpp
void open_half_memory(const std::filesystem::path& path,
                      PagedProteinAAStore& store,
                      PagedProteinAAStore::ResidentCachePolicy policy,
                      uint32_t page_hot_min_refs = 32) {
    PagedProteinAAStore::Options options;
    options.memory_bytes = 2 * 4096;
    options.prefetch_bytes = 128ULL * 1024ULL * 1024ULL;
    options.io_threads = 2;
    options.cache_policy = policy;
    options.page_hot_lru_min_refs = page_hot_min_refs;
    options.io_backend = PagedProteinAAStore::PrefetchIoBackend::Pread;
    options.auto_preload = true;
    if (!store.open(path.string(), options)) {
        throw std::runtime_error("failed to open half-memory regression store");
    }
}
```

加入两个测试块：

```cpp
{
    PagedProteinAAStore store;
    open_half_memory(temp.path(), store,
                     PagedProteinAAStore::ResidentCachePolicy::PageHotLru, 32);
    store.reset_stats();
    auto fetched = store.fetch({2});
    expect_fetch_matches(fetched, {2}, sequences);
    auto stats = store.stats();
    expect(stats.lru_page_hot_candidate_pages == 0,
           "low-density pages should not become page-hot candidates");
    expect(stats.lru_admission_first_misses == 0 &&
               stats.lru_admission_promotions == 0 && stats.evictions == 0,
           "low-density pages should not enter 2Q admission");
}

{
    PagedProteinAAStore store;
    open_half_memory(temp.path(), store,
                     PagedProteinAAStore::ResidentCachePolicy::PageHotLru, 2);
    std::vector<size_t> hot_ids(2, 2);
    store.reset_stats();
    expect_fetch_matches(store.fetch(hot_ids), hot_ids, sequences);
    expect(store.stats().lru_admission_first_misses > 0,
           "first hot batch should populate the 2Q recent set");
    expect_fetch_matches(store.fetch(hot_ids), hot_ids, sequences);
    expect(store.stats().lru_admission_promotions > 0,
           "second hot batch should promote page-hot pages");
}
```

- [ ] **Step 2: 运行行为测试并检查 freeze 代码仍存在**

Run:

```bash
cmake --build /tmp/aastore-halfmem-build --target paged_batch_regression -j4
/tmp/aastore-halfmem-build/paged_batch_regression
```

Expected: 至少一个 page-hot/2Q 断言失败，或 freeze 相关接口仍存在导致后续编译删除检查失败。

- [ ] **Step 3: 删除 freeze 数据与 API**

从 `PagedProteinAAStore.hpp` 删除：

```cpp
Options::lru_freeze_coverage_threshold
CacheStats::lru_admission_frozen_batches
CacheStats::lru_admission_frozen_pages
set_lru_freeze_coverage_threshold()
lru_freeze_coverage_threshold_
lru_admissions_frozen_
should_freeze_lru_admissions_for_batch()
```

删除 `configure()` 和 `clear_cache()` 中对应操作，并从 `PagedAAStoreTools.cpp` 删除 frozen counters 输出。

- [ ] **Step 4: 删除 range builder 的 freeze 分支**

删除 `freeze_lru_admissions`，admission 条件改为：

```cpp
if (page_hot_candidate && allow_lru_admission && lru_admission_epoch == 0) {
    lru_admission_epoch = next_lru_admission_epoch();
}
const bool admit_to_lru = lru_admission_epoch != 0 &&
    page_hot_candidate && lru_should_admit_page(page_id, lru_admission_epoch);
```

- [ ] **Step 5: 运行完整 C++ 回归并检查 freeze 已删除**

Run:

```bash
cmake --build /tmp/aastore-halfmem-build --target paged_aatools paged_batch_regression -j4
/tmp/aastore-halfmem-build/paged_batch_regression
rg "freeze_lru|lru_admissions_frozen|lru_admission_frozen" \
   src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp
```

Expected: regression 输出 `paged batch regression: ok`；`rg` 无匹配。

- [ ] **Step 6: 提交 admission 行为改动**

```bash
git add src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp \
        src/paged_batch_regression.cpp
git commit -m "Remove LRU freeze in favor of page-hot admission"
```

---

### Task 3: Python 冒烟与 10M 半内存冷盘验证

**Files:**
- Verify: `python/aastore_backend.py`
- Verify: `src/PagedAAStoreTools.cpp`
- Create: `halfmem_overlap_20260710/page_hot_lru_after/*.txt`
- Modify: `halfmem_overlap_20260710/README.md`

**Interfaces:**
- Consumes: `--cache-policy page_hot_lru`
- Consumes: legacy `--cache-policy lru`
- Produces: checksum-equivalent benchmark records

- [ ] **Step 1: 构建当前 C API 并运行 Python 策略冒烟**

Run:

```bash
cmake --build /tmp/aastore-halfmem-build --target aastore_c -j4
PYTHONPATH=python AASTORE_C_LIB=/tmp/aastore-halfmem-build/libaastore_c.so \
.venv_pyfastx/bin/python -c 'import aastore; db=aastore.open("/home/bigssd/gaoyijie_aastore_batch_prefetch_20260710/paged_store_10m", memory_mb=0); print(len(db), db[0][:10])'
```

Expected: 打印 `10000000` 和非空序列前缀，无异常。

- [ ] **Step 2: 验证 canonical 与兼容 CLI 名称**

Run:

```bash
/tmp/aastore-halfmem-build/paged_aatools bench \
  -i /home/bigssd/gaoyijie_aastore_batch_prefetch_20260710/paged_store_10m \
  -n 1000 --memory-mb 1072 --fetch --threads 4 --cache-policy lru
```

Expected: 输出 `cache_policy=page_hot_lru`。

- [ ] **Step 3: 运行三轮 page_hot_lru 冷盘基准**

每轮运行：

```bash
numactl --cpunodebind=0 --membind=0 \
  /tmp/aastore-halfmem-build/paged_aatools bench \
  -i /home/bigssd/gaoyijie_aastore_batch_prefetch_20260710/paged_store_10m \
  -n 1000000 --rounds 1 --seed 20260710 \
  --memory-mb 1072 --fetch --prefetch-mb 128 \
  --threads 4 --prefetch-depth 64 \
  --cache-policy page_hot_lru --page-hot-lru-min-refs 32 \
  --drop-page-cache --io-backend uring
```

Expected:

- 三轮 `real_get_chars=360236110`；
- 三轮 `checksum_dec=27395636418`；
- `lru_page_hot_filtered_pages > 0`；
- freeze 字段不再输出；
- admission/eviction 数量受 32 引用阈值限制。

- [ ] **Step 4: 更新实验说明**

在 `halfmem_overlap_20260710/README.md` 中替换旧 LRU freeze 小节，记录 canonical `page_hot_lru`、候选 page 数、admission/eviction 和三轮中位数，并与 prefix `175.8 ms` 基线比较。

- [ ] **Step 5: 最终验证**

Run:

```bash
git diff --check
/tmp/aastore-halfmem-build/paged_batch_regression
rg "ResidentCachePolicy::Lru|freeze_lru|lru_admissions_frozen|lru_admission_frozen" \
   src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp src/paged_batch_regression.cpp
```

Expected: diff check 和 regression 通过；`rg` 无匹配。

- [ ] **Step 6: 提交验证与实验记录**

```bash
git add halfmem_overlap_20260710/README.md \
        halfmem_overlap_20260710/page_hot_lru_after
git commit -m "Benchmark canonical page-hot LRU policy"
```
