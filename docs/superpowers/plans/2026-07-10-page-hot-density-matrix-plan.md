# Page-hot 请求密度合成矩阵实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 metadata-only 离线分析器，在现有 10M/100M AAStore 上量化请求加权 page 密度、2Q 二次确认和预计替换规模。

**Architecture:** 纯头文件核心负责 page demand 累计、密度判断和 8-epoch 2Q 模拟；独立 CLI 通过 AAStore 公共 metadata API 流式生成合成请求，不保存完整请求数组。分析器与正式缓存路径隔离，结果写入当前仓库的独立实验目录。

**Tech Stack:** C++17、header-only `PagedProteinAAStore`、SplitMix64、compact/legacy metadata、独立回归程序、Linux `/usr/bin/time`。

## Global Constraints

- 不修改 `PagedProteinAAStore`、现有 CLI、C API 或 Python 正式行为。
- 不读取或解码 `pages.bin` payload。
- 重复 sequence ID 的 page overlap 必须重复累计。
- 尾页密度必须使用实际 page residues。
- 2Q 窗口固定为 8 批；第一次满足只记录，窗口内第二次满足才 promotion。
- 不实现 admission budget，只报告 promotion 数和估算替换字节。
- 所有 workload 使用固定 seed `20260710`。
- 结果只写入 `page_hot_density_matrix_20260710/`。
- 不回退或提交与本实验无关的 dirty worktree 修改。

---

### Task 1: Page demand 与 2Q 纯计算核心

**Files:**
- Create: `src/PageHotDensityMatrix.hpp`
- Create: `src/page_hot_density_matrix_regression.cpp`

**Interfaces:**
- Produces: `page_hot_density::DensePageDemand`
- Produces: `page_hot_density::density_meets_percent(uint64_t, uint64_t, uint32_t)`
- Produces: `page_hot_density::DensityBuckets`
- Produces: `page_hot_density::TwoQTracker`

- [ ] **Step 1: 写失败测试**

测试必须覆盖：两个重复 chunk 的 `20 + 30 = 50` demand、100-residue
page 的 50% 边界、40-residue 尾页的 50% 边界、200% density 桶、2Q
第一次不 promotion、窗口内第二次 promotion、超过 8 epoch 不 promotion，以及
只清理 touched page。

- [ ] **Step 2: 编译并确认 RED**

```bash
c++ -std=c++17 -O2 -Isrc src/page_hot_density_matrix_regression.cpp \
  -o /tmp/page_hot_density_matrix_regression
```

Expected: `PageHotDensityMatrix.hpp` 不存在导致编译失败。

- [ ] **Step 3: 实现纯计算接口**

```cpp
namespace page_hot_density {

class DensePageDemand {
public:
    explicit DensePageDemand(size_t page_count);
    void add(uint64_t page_id, uint64_t residues);
    uint64_t residues(uint64_t page_id) const;
    uint32_t refs(uint64_t page_id) const;
    const std::vector<uint32_t>& touched_pages() const;
    void clear_touched();
};

bool density_meets_percent(uint64_t demand_residues,
                           uint64_t page_residues,
                           uint32_t threshold_percent);

struct DensityBuckets {
    uint64_t lt_25, ge_25_lt_50, ge_50_lt_75;
    uint64_t ge_75_lt_100, ge_100_lt_200, ge_200;
};

DensityBuckets classify_density(uint64_t demand_residues,
                                uint64_t page_residues);

class TwoQTracker {
public:
    TwoQTracker(size_t page_count, uint16_t window_epochs);
    bool observe(uint64_t page_id, uint16_t epoch);
    bool promoted(uint64_t page_id) const;
};

}  // namespace page_hot_density
```

密度比较使用 `__uint128_t` 交叉相乘。`DensePageDemand::add()` 只在 page
首次从 0 变为非零时追加 touched ID；`clear_touched()` 只清 touched page。

- [ ] **Step 4: 运行 GREEN 与 sanitizer**

```bash
c++ -std=c++17 -O2 -Isrc src/page_hot_density_matrix_regression.cpp \
  -o /tmp/page_hot_density_matrix_regression
/tmp/page_hot_density_matrix_regression
c++ -std=c++17 -O1 -g -fsanitize=address,undefined -Isrc \
  src/page_hot_density_matrix_regression.cpp \
  -o /tmp/page_hot_density_matrix_regression_asan
/tmp/page_hot_density_matrix_regression_asan
```

Expected: 两次均输出 `page hot density regression: ok`。

- [ ] **Step 5: 提交核心**

```bash
git add src/PageHotDensityMatrix.hpp src/page_hot_density_matrix_regression.cpp
git commit -m "Add page-hot density analysis core"
```

---

### Task 2: Metadata-only 合成 workload 分析器

**Files:**
- Modify: `src/PageHotDensityMatrix.hpp`
- Modify: `src/page_hot_density_matrix_regression.cpp`
- Create: `src/page_hot_density_matrix.cpp`

**Interfaces:**
- Consumes: `DensePageDemand`, `density_meets_percent`, `TwoQTracker`
- Produces: `SyntheticIdGenerator`
- Produces CLI: `page_hot_density_matrix --store PATH --requests N --rounds N --workload NAME --seed N --output PATH`
- Workloads: `uniform_fresh`, `uniform_repeat`, `hot80_20`, `phase_shift`

- [ ] **Step 1: 写 workload 失败测试**

回归程序用两个相同 seed 的 `SyntheticIdGenerator` 比较前 10,000 个 uniform
和 hot ID，要求完全一致；还要验证热点池 A/B 不重叠。热点池使用与
`store_size` 互质的仿射置换，把 rank 区间 `[0,1%)` 和 `[1%,2%)`
分散映射到全库 ID 空间。

- [ ] **Step 2: 编译并确认 RED**

使用 Task 1 编译命令。Expected: `SyntheticIdGenerator` 不存在导致失败。

- [ ] **Step 3: 实现 workload 生成与 CLI**

分析器按 metadata-only 方式打开 store：

```cpp
PagedProteinAAStore::Options options;
options.memory_bytes = 1;
options.prefetch_bytes = 0;
options.auto_preload = false;
options.meta_open_mode = PagedProteinAAStore::MetaOpenMode::Mmap;
PagedProteinAAStore store;
if (!store.open(store_path, options)) {
    throw std::runtime_error("failed to open store metadata");
}
```

每个请求用 `sequence_start_unchecked()` 和
`sequence_length_unchecked()` 切成 page overlap。尾页实际长度为：

```cpp
std::min<uint64_t>(store.page_residues(),
    store.total_length() - page_id * store.page_residues())
```

每段 overlap 调用 `demand.add(page_id, take)`。四种 workload 定义：

- `uniform_fresh`：每轮使用不同 SplitMix64 seed；
- `uniform_repeat`：所有轮使用同一 seed；
- `hot80_20`：每 5 个请求中 4 个来自固定 1% 热点池 A，1 个全库均匀；
- `phase_shift`：前两轮用 A，后两轮用不重叠热点池 B，均为 80/20。

输出稳定 `key=value` 行，包含每轮 density 25/50/75/100、refs 2/8/32、
first misses、promotions、估算替换字节和下一轮覆盖率。主输出不包含墙钟
时间、PID 或时间戳；性能计时由 `/usr/bin/time` 写入独立 sidecar。

- [ ] **Step 4: 构建并运行小 store smoke**

```bash
c++ -std=c++17 -O3 -DNDEBUG -march=native -pthread -Isrc \
  src/page_hot_density_matrix.cpp -o /tmp/page_hot_density_matrix \
  -luring -lstdc++fs
/tmp/page_hot_density_matrix \
  --store rabbityclust_compat_test/small.paged \
  --requests 10000 --rounds 4 --workload uniform_repeat \
  --seed 20260710 --output /tmp/page_hot_density_smoke.txt
```

Expected: exit 0，输出包含四轮及全部阈值和 2Q 指标。

- [ ] **Step 5: 验证 metadata-only 与可复现性**

```bash
strace -f -e trace=pread64,read,mmap \
  /tmp/page_hot_density_matrix \
  --store rabbityclust_compat_test/small.paged \
  --requests 1000 --rounds 2 --workload uniform_fresh \
  --seed 20260710 --output /tmp/page_hot_density_trace.txt
/tmp/page_hot_density_matrix \
  --store rabbityclust_compat_test/small.paged \
  --requests 10000 --rounds 4 --workload hot80_20 \
  --seed 20260710 --output /tmp/page_hot_density_a.txt
/tmp/page_hot_density_matrix \
  --store rabbityclust_compat_test/small.paged \
  --requests 10000 --rounds 4 --workload hot80_20 \
  --seed 20260710 --output /tmp/page_hot_density_b.txt
cmp /tmp/page_hot_density_a.txt /tmp/page_hot_density_b.txt
```

Expected: `pages.bin` 无 payload read；`cmp` exit 0。

- [ ] **Step 6: 提交分析器**

```bash
git add src/PageHotDensityMatrix.hpp src/page_hot_density_matrix.cpp \
        src/page_hot_density_matrix_regression.cpp
git commit -m "Add metadata-only page-hot density analyzer"
```

---

### Task 3: 运行 10M/100M 合成矩阵并总结

**Files:**
- Create: `page_hot_density_matrix_20260710/*.txt`
- Create: `page_hot_density_matrix_20260710/summary.tsv`
- Create: `page_hot_density_matrix_20260710/README.md`

**Interfaces:**
- Consumes: `/tmp/page_hot_density_matrix`
- Produces: 8 个 workload 日志、汇总表和结论

- [ ] **Step 1: 运行 10M 四类 workload**

分别用 `uniform_fresh`、`uniform_repeat`、`hot80_20`、`phase_shift`
替换 `WORKLOAD`：

```bash
/usr/bin/time -v -o page_hot_density_matrix_20260710/10m_WORKLOAD.time.txt \
  /tmp/page_hot_density_matrix \
  --store /home/user_home/gaoyijie/build_eval_current/10m_store \
  --requests 1000000 --rounds 4 --workload WORKLOAD \
  --seed 20260710 \
  --output page_hot_density_matrix_20260710/10m_WORKLOAD.txt
```

Expected: 四个命令 exit 0。

- [ ] **Step 2: 运行 100M 四类 workload**

对四种 workload 分别运行：

```bash
/usr/bin/time -v -o page_hot_density_matrix_20260710/100m_WORKLOAD.time.txt \
  /tmp/page_hot_density_matrix \
  --store /home/user_home/gaoyijie/build_eval_current/100m_store \
  --requests 10000000 --rounds 4 --workload WORKLOAD \
  --seed 20260710 \
  --output page_hot_density_matrix_20260710/100m_WORKLOAD.txt
```

Expected: 四个命令 exit 0；两组均抽取数据库序列数的 10%。

- [ ] **Step 3: 生成汇总表并检查单调性**

`summary.tsv` 每行字段：

```text
dataset workload round touched_pages density25 density50 density75 density100
refs2 refs8 refs32 first_miss50 promotions50 promoted_bytes50
future_page_ref_coverage50 future_residue_coverage50 wall_seconds peak_rss_kib
```

逐行检查：

```text
density25 >= density50 >= density75 >= density100
refs2 >= refs8 >= refs32
promotions50 <= density50
```

- [ ] **Step 4: 写 README 结论**

README 必须回答：Uniform Fresh 的误 promotion、Uniform Repeat 是否产生大范围
晋升、80/20 用多少 promotion page 覆盖多少下一轮 demand、Phase Shift 第三轮
是否避免立即晋升新热点、25/50/75 哪档适合作为默认值，以及是否需要
admission budget。

- [ ] **Step 5: 最终验证**

```bash
/tmp/page_hot_density_matrix_regression
git diff --check
rg -n "TBD|TODO|nan|inf" page_hot_density_matrix_20260710
```

Expected: regression 通过；diff check 无输出；结果无占位符或非法数值。

- [ ] **Step 6: 提交代码与结果**

```bash
git add src/PageHotDensityMatrix.hpp src/page_hot_density_matrix.cpp \
        src/page_hot_density_matrix_regression.cpp \
        page_hot_density_matrix_20260710
git commit -m "Evaluate request-weighted page-hot density"
```
