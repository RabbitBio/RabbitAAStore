# HMMER esl-sfetch AAStore Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Easel `esl-sfetch` 增加保持名称列表顺序的 AAStore 批量取序后端，并与原生 FASTA/SSI 后端做冷启动、内存和正确性对比。

**Architecture:** AAStore 提供稳定 C 头文件和批量名称解析 ABI；Easel 侧新增一个小型后端模块，负责解析名称文件、一次 arena fetch 和规范化 FASTA 输出。上游 `esl-sfetch.c` 只增加选项和早期分派，普通 FASTA/SSI 路径保持原样。

**Tech Stack:** C++17、C11、CMake、AAStore C ABI、HMMER/Easel、Python 3 回归与基准脚本。

## Global Constraints

- 不改变 AAStore page、meta、compact meta 和名称索引格式。
- 不修改 HMMER 搜索算法和原有 FASTA/SSI 路径。
- AAStore 模式只支持完整蛋白质序列；`-c`、`-C`、`-r`、`--index` 和 `--informat` 明确拒绝。
- `-f` 模式保持输入名称顺序，并与原版一样拒绝重复名称。
- 正确性比较主名称、顺序和 residues，不要求 FASTA 描述字段或换行字节一致。
- 大型构建产物放在 `/home/bigssd/gaoyijie_hmmer_sfetch_aastore_20260716`，源码、脚本和结果摘要保留在当前仓库。
- 不修改或回退仓库中与本任务无关的 dirty worktree 文件。

---

### Task 1: 稳定 AAStore C ABI 与批量名称解析

**Files:**
- Create: `include/aastore_c.h`
- Modify: `src/aastore_c_api.cpp`
- Create: `tests/aastore_c_api_regression.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: opaque `AAStoreCHandle`
- Produces: `int aastore_batch_id_of(AAStoreCHandle*, const char* const*, uint64_t, uint64_t*, uint64_t*)`
- Preserves: all existing `aastore_open/get/batch_layout/batch_get_arena_prepared` symbols

- [ ] **Step 1: 写失败的 C ABI 回归**

创建三条临时序列和 `seq_names.txt`，用公开头文件打开 store，并断言：

```cpp
const char* names[] = {"gamma", "alpha"};
uint64_t ids[2] = {};
uint64_t missing = UINT64_MAX;
expect(aastore_batch_id_of(handle, names, 2, ids, &missing) == 0,
       "batch name lookup should succeed");
expect(ids[0] == 2 && ids[1] == 0,
       "batch name lookup should preserve request order");

const char* missing_names[] = {"alpha", "absent"};
expect(aastore_batch_id_of(handle, missing_names, 2, ids, &missing) == 1,
       "missing name should return one");
expect(missing == 1, "missing index should identify the failed request");
```

- [ ] **Step 2: 构建并确认 RED**

Run:

```bash
cmake -S . -B build-tests -DAASTORE_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests --target aastore_c_api_regression -j4
```

Expected: 编译失败，提示 `include/aastore_c.h` 或 `aastore_batch_id_of` 不存在。

- [ ] **Step 3: 增加公开头和最小批量解析实现**

头文件使用 C/C++ 兼容声明：

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAStoreCHandle AAStoreCHandle;

int aastore_batch_id_of(AAStoreCHandle *handle,
                        const char *const *names,
                        uint64_t count,
                        uint64_t *out_ids,
                        uint64_t *missing_index);

#ifdef __cplusplus
}
#endif
```

实现按输入顺序调用不可变名称索引；首个缺失名称返回 `1` 并填写位置，异常返回 `-2`：

```cpp
for (uint64_t i = 0; i < count; ++i) {
    size_t id = 0;
    if (names[i] == nullptr || !handle->store.try_id_of(names[i], id)) {
        if (missing_index != nullptr) *missing_index = i;
        return 1;
    }
    out_ids[i] = static_cast<uint64_t>(id);
}
return 0;
```

让 `src/aastore_c_api.cpp` include 公开头，并让 CMake 将 `include/` 暴露给消费者。

- [ ] **Step 4: 验证 GREEN 和旧回归**

Run:

```bash
cmake --build build-tests --target aastore_c_api_regression paged_batch_regression -j4
./build-tests/aastore_c_api_regression
./build-tests/paged_batch_regression
```

Expected: 分别输出 `aastore C API regression: ok` 和 `paged batch regression: ok`。

- [ ] **Step 5: 提交 C ABI 改动**

```bash
git add include/aastore_c.h src/aastore_c_api.cpp tests/aastore_c_api_regression.cpp CMakeLists.txt
git commit -m "Expose batch name lookup in the AAStore C API"
```

---

### Task 2: Easel AAStore 批量取序后端

**Files:**
- Create: `integrations/hmmer/esl_sfetch_aastore.h`
- Create: `integrations/hmmer/esl_sfetch_aastore.c`
- Create: `tests/esl_sfetch_aastore_regression.py`

**Interfaces:**
- Consumes: `include/aastore_c.h`
- Produces: `int esl_sfetch_aastore_run(ESL_GETOPTS*, FILE*, const char*, char*, size_t)`
- Produces: `AASTORE_TIMING open_ms=... resolve_ms=... layout_ms=... fetch_ms=... output_ms=...`

- [ ] **Step 1: 写端到端失败测试**

测试创建小型 FASTA、AAStore 和固定名称列表，然后执行：

```python
native = run([original, "-f", fasta, ids_file])
actual = run([patched, "--aastore", "-f", store, ids_file])
assert parse_fasta(actual.stdout) == parse_fasta(native.stdout)
```

再断言重复名称和未知名称返回非零退出码，stderr 包含名称及输入行号。

- [ ] **Step 2: 运行并确认 RED**

Run:

```bash
python3 tests/esl_sfetch_aastore_regression.py --help
```

Expected: 失败，因为回归脚本或 AAStore 版 `esl-sfetch` 尚不存在。

- [ ] **Step 3: 实现名称收集与 AAStore batch fetch**

后端按以下固定顺序调用 ABI：

```c
handle = aastore_open(store_dir, memory_mb, prefetch_mb,
                      "page_hot_lru", names_path, "compact");
rc = aastore_batch_id_of(handle, keys, count, ids, &missing_index);
rc = aastore_batch_layout_starts(handle, ids, count,
                                 starts, offsets, lengths, &arena_bytes);
arena = malloc((size_t)arena_bytes);
rc = aastore_batch_get_arena_prepared(handle, ids, count, arena,
                                      arena_bytes, starts, offsets, lengths);
```

名称文件使用 `ESL_FILEPARSER`，以 `ESL_KEYHASH` 保持原版重复检测。规范化输出为：

```c
fprintf(ofp, ">%s\n", keys[i]);
for (uint64_t pos = 0; pos < lengths[i]; pos += 60) {
    size_t width = (size_t)((lengths[i] - pos) < 60 ? lengths[i] - pos : 60);
    fwrite(arena + offsets[i] + pos, 1, width, ofp);
    fputc('\n', ofp);
}
```

所有分配、AAStore 错误和输出错误走单一 cleanup 路径；`--aastore-stats` 只向 stderr 输出计时，不污染 FASTA stdout。

- [ ] **Step 4: 编译后端对象并做静态检查**

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror \
  -Iinclude \
  -Iexperiments/hmmer_esl_sfetch_20260716/hmmer/easel \
  -c integrations/hmmer/esl_sfetch_aastore.c \
  -o /tmp/esl_sfetch_aastore.o
```

Expected: 编译成功且无 warning。

- [ ] **Step 5: 提交后端与失败测试**

```bash
git add integrations/hmmer/esl_sfetch_aastore.h \
        integrations/hmmer/esl_sfetch_aastore.c \
        tests/esl_sfetch_aastore_regression.py
git commit -m "Add the Easel AAStore sequence fetch backend"
```

---

### Task 3: 接入 esl-sfetch 并完成正确性回归

**Files:**
- Create: `integrations/hmmer/esl-sfetch-aastore.patch`
- Create: `integrations/hmmer/build_esl_sfetch_aastore.sh`
- Create: `integrations/hmmer/README.md`

**Interfaces:**
- Consumes: `esl_sfetch_aastore_run(...)`
- Produces: `esl-sfetch --aastore [-f] STORE KEY_OR_FILE`

- [ ] **Step 1: 给上游 options 和 main 增加最小分派 patch**

新增选项：

```c
{ "--aastore",             eslARG_NONE, FALSE, NULL, NULL, NULL, NULL, NULL, "read sequences from an AAStore directory", 3 },
{ "--aastore-memory-mb",   eslARG_INT,  "0",   NULL, "n>=-1", NULL, "--aastore", NULL, "AAStore resident memory in MiB", 3 },
{ "--aastore-prefetch-mb", eslARG_INT,  "128", NULL, "n>=0",  NULL, "--aastore", NULL, "AAStore prefetch memory in MiB", 3 },
{ "--aastore-names",       eslARG_STRING,FALSE, NULL, NULL, NULL, "--aastore", NULL, "override seq_names.txt", 3 },
{ "--aastore-stats",       eslARG_NONE, FALSE, NULL, NULL, NULL, "--aastore", NULL, "print AAStore timing to stderr", 3 },
```

在普通 `esl_sqfile_Open()` 前检测 `--aastore`，验证参数组合，打开 `-o/-O` 输出后调用后端并退出。非 AAStore 分支继续执行原代码。

- [ ] **Step 2: 实现可复现构建脚本**

脚本固定记录 HMMER 和 Easel commit，复制后端源码、应用 patch，然后使用：

```bash
autoreconf -i
./configure
make -j"${JOBS:-4}"
make -C easel/miniapps esl-sfetch \
  AASTORE_ROOT="$AASTORE_ROOT" \
  AASTORE_BUILD="$AASTORE_BUILD"
```

输出原版和接入版二进制到 `experiments/hmmer_esl_sfetch_20260716/bin/`，并写入对应 commit 和 `libaastore_c.so` SHA256。

- [ ] **Step 3: 构建并运行 GREEN 回归**

Run:

```bash
cmake --build build-tests --target paged_aatools aastore_c -j4
bash integrations/hmmer/build_esl_sfetch_aastore.sh \
  experiments/hmmer_esl_sfetch_20260716/hmmer build-tests
python3 tests/esl_sfetch_aastore_regression.py \
  --original experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-original \
  --patched experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-aastore \
  --paged-tools build-tests/paged_aatools
```

Expected: 输出 `esl-sfetch AAStore regression: ok`，单条、多条和错误路径全部通过。

- [ ] **Step 4: 运行原版 Easel 测试并检查普通路径**

Run:

```bash
make -C experiments/hmmer_esl_sfetch_20260716/hmmer/easel/miniapps check
experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-aastore -f \
  tests/data/esl_sfetch_input.fa tests/data/esl_sfetch_ids.txt >/tmp/esl-native-path.fa
```

Expected: Easel miniapp 测试通过；不带 `--aastore` 时继续走 FASTA/SSI 路径。

- [ ] **Step 5: 提交上游接入与文档**

```bash
git add integrations/hmmer/esl-sfetch-aastore.patch \
        integrations/hmmer/build_esl_sfetch_aastore.sh \
        integrations/hmmer/README.md
git commit -m "Integrate AAStore with Easel esl-sfetch"
```

---

### Task 4: 冷启动、内存与 HMMER 管道对比

**Files:**
- Create: `benchmarks/hmmer_sfetch_compare.py`
- Create: `experiments/hmmer_esl_sfetch_20260716/README.md`
- Create: `experiments/hmmer_esl_sfetch_20260716/results/*.json`

**Interfaces:**
- Consumes: original and AAStore `esl-sfetch` binaries
- Produces: fixed-ID cold benchmark JSON and HMMER pipe correctness summary

- [ ] **Step 1: 写公平对比驱动**

每个 backend 使用独立进程；测试前分别对 FASTA、SSI、`pages.bin`、compact meta、
名称文件和名称索引调用 `POSIX_FADV_DONTNEED`。计时区间包含 open、查名、fetch 和
完整 `/dev/null` 输出，不包含 ID 生成和 cache drop。

结果至少记录：

```json
{
  "backend": "aastore_no_memory",
  "requests": 100000,
  "seconds": 0.0,
  "peak_rss_mib": 0.0,
  "output_bytes": 0,
  "sequence_checksum": "sha256"
}
```

- [ ] **Step 2: 构建 1M 带名称索引的 AAStore 与 SSI**

Run:

```bash
mkdir -p /home/bigssd/gaoyijie_hmmer_sfetch_aastore_20260716
build-tests/paged_aatools build-fa \
  -f /home/bigssd/test_cdhit/1m_nr.fa \
  -o /home/bigssd/gaoyijie_hmmer_sfetch_aastore_20260716/store_1m \
  --threads 8 --name-index
experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-original \
  --index /home/bigssd/test_cdhit/1m_nr.fa
```

Expected: store 含 `pages.bin`、compact meta、`seq_names.txt` 和 `seq_names.index.bin`；FASTA 含 `.ssi`。

- [ ] **Step 3: 跑 1M 库三组冷启动矩阵**

Run:

```bash
python3 benchmarks/hmmer_sfetch_compare.py \
  --fasta /home/bigssd/test_cdhit/1m_nr.fa \
  --store /home/bigssd/gaoyijie_hmmer_sfetch_aastore_20260716/store_1m \
  --counts 10000,100000,1000000 --runs 3 \
  --output experiments/hmmer_esl_sfetch_20260716/results/cold_1m.json
```

Expected: 所有 backend 的名称、顺序、总 residues 和 checksum 一致；JSON 包含三轮原始值和中位数。

- [ ] **Step 4: 跑 10M 库主结果**

使用 `/home/bigssd/test_cdhit/10m_nr.fa` 构建带名称索引的 store，重复 10K、100K、1M 请求，比较原版 SSI、AAStore no-memory 和 AAStore half-memory。

Expected: 结果正确，记录每种模式的时间、峰值 RSS、索引大小和 AAStore 内部分项计时。

- [ ] **Step 5: 运行 hmmsearch 管道冒烟**

从 HMMER tutorial 选择一个蛋白 HMM 和固定候选 ID 列表，分别执行：

```bash
esl-sfetch-original -f proteins.fa ids.txt | hmmsearch model.hmm -
esl-sfetch-aastore --aastore -f store ids.txt | hmmsearch model.hmm -
```

Expected: 命中 target 名称、domain 数量和 score/e-value 字段一致。

- [ ] **Step 6: 汇总结果并最终验证**

Run:

```bash
git diff --check
./build-tests/aastore_c_api_regression
./build-tests/paged_batch_regression
python3 tests/esl_sfetch_aastore_regression.py \
  --original experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-original \
  --patched experiments/hmmer_esl_sfetch_20260716/bin/esl-sfetch-aastore \
  --paged-tools build-tests/paged_aatools
```

Expected: diff check 与三个回归全部通过；README 明确区分构建成本、冷 open、取序和管道总时间。

- [ ] **Step 7: 提交基准与结果摘要**

```bash
git add benchmarks/hmmer_sfetch_compare.py \
        experiments/hmmer_esl_sfetch_20260716/README.md \
        experiments/hmmer_esl_sfetch_20260716/results
git commit -m "Benchmark esl-sfetch with the AAStore backend"
```
