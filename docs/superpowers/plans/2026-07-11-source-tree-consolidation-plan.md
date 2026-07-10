# AAStore 源码目录收敛实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除旧 `ProteinAAStore.hpp` 依赖，并把非生产源码归档，使默认构建只包含最新 paged AAStore。

**Architecture:** 先把低 5-bit 编码映射收回公共 encoder，删除旧 store 转换 API 和 CLI；再机械移动测试、benchmark、legacy、experiment 文件并收敛 CMake。归档文件不参与默认构建，paged 格式和运行 API 不变。

**Tech Stack:** C++17、CMake、CLI11、header-only `PagedProteinAAStore`、现有 regression、Python ctypes smoke。

## Global Constraints

- 永久删除 `src/ProteinAAStore.hpp`。
- 默认只构建 `paged_aatools` 与 `aastore_c`。
- `build-fa`、open/get/fetch、C/Python API 和 paged 文件格式不变。
- 旧 `build` 子命令和 `build_from_store()` 有意删除。
- 归档文件不参与默认构建。
- 不回退无关 dirty worktree 修改。

---

### Task 1: 解除旧 ProteinAAStore 编译依赖

**Files:**
- Modify: `src/Packed5BitEncoder.hpp`
- Modify: `src/PagedProteinAAStore.hpp`
- Modify: `src/PagedAAStoreTools.cpp`
- Modify: `src/paged_batch_regression.cpp`
- Delete: `src/ProteinAAStore.hpp`

**Interfaces:**
- Produces: `aastore_detail::encode_aa_5bit_fast(char) -> uint8_t`
- Removes: `PagedProteinAAStore::build_from_store()`
- Removes: CLI subcommand `build`

- [ ] **Step 1: 在 regression 加入新编码接口断言**

```cpp
expect(aastore_detail::encode_aa_5bit_fast('A') ==
           (static_cast<uint8_t>('A') & 0x1F),
       "fast encoder should preserve the legacy low-five-bit mapping");
```

- [ ] **Step 2: 构建并确认 RED**

```bash
cmake --build /tmp/aastore-halfmem-build --target paged_batch_regression -j4
```

Expected: `encode_aa_5bit_fast` 不存在导致编译失败。

- [ ] **Step 3: 实现公共映射并移除旧接口**

在 `Packed5BitEncoder.hpp` 增加：

```cpp
static inline uint8_t encode_aa_5bit_fast(char c) {
    return static_cast<uint8_t>(c) & 0x1F;
}
```

将 `PagedProteinAAStore.hpp` 两处 `ProteinAAStore::enc()` 替换为该函数，删除
旧 include 和完整 `build_from_store()`。删除 `PagedAAStoreTools.cpp` 的 `build`
subcommand、输入变量和处理分支，只保留 `build-fa`。

- [ ] **Step 4: 删除旧头并验证 GREEN**

```bash
rm src/ProteinAAStore.hpp
cmake --build /tmp/aastore-halfmem-build --target paged_batch_regression paged_aatools aastore_c -j4
/tmp/aastore-halfmem-build/paged_batch_regression
rg -n 'ProteinAAStore.hpp|ProteinAAStore::|build_from_store' \
  src/PagedProteinAAStore.hpp src/PagedAAStoreTools.cpp src/Packed5BitEncoder.hpp
```

Expected: 构建通过，regression 输出 `paged batch regression: ok`，`rg` 无匹配。

---

### Task 2: 归档非生产源码并收敛默认构建

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Move: `src/paged_batch_regression.cpp` -> `tests/paged_batch_regression.cpp`
- Move: `src/paged_prefetch_verify.cpp` -> `tests/paged_prefetch_verify.cpp`
- Move: benchmark、legacy、experiment 文件到设计指定目录

**Interfaces:**
- Produces: CMake option `AASTORE_BUILD_TESTS`, default `OFF`
- Default targets: `paged_aatools`, `aastore_c`

- [ ] **Step 1: 机械移动文件**

创建 `tests/`、`benchmarks/`、`legacy/`、`experiments/page_hot_density/`，按设计
文档中的精确映射移动文件。移动后 `src/` 应只剩 7 个生产文件。

- [ ] **Step 2: 收敛 CMake**

删除旧 target，仅保留生产 target。增加：

```cmake
option(AASTORE_BUILD_TESTS "Build AAStore regression targets" OFF)
if(AASTORE_BUILD_TESTS)
  add_executable(paged_batch_regression tests/paged_batch_regression.cpp)
  target_link_libraries(paged_batch_regression PRIVATE Threads::Threads)
  target_include_directories(paged_batch_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
```

检测到 liburing 时，对生产工具及条件测试 target 保留现有 compile definition、
include 和 link 设置。

- [ ] **Step 3: 更新 README**

工具列表只公开 `paged_aatools` 与 `aastore_c`，构建示例使用 `build-fa`；说明
旧工具位于 `legacy/`/`benchmarks/` 且不参与默认构建。

- [ ] **Step 4: 全新默认构建**

```bash
cmake -S . -B /tmp/aastore-clean-core -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/aastore-clean-core -j4
cmake --build /tmp/aastore-clean-core --target help
```

Expected: 默认构建成功；项目 target 只有 `paged_aatools` 和 `aastore_c`。

- [ ] **Step 5: 测试构建与功能 smoke**

```bash
cmake -S . -B /tmp/aastore-clean-tests -DAASTORE_BUILD_TESTS=ON
cmake --build /tmp/aastore-clean-tests --target paged_batch_regression paged_aatools aastore_c -j4
/tmp/aastore-clean-tests/paged_batch_regression
```

再用小 FASTA 执行 `build-fa` 与 `get`，并用新 `libaastore_c.so` 运行 Python
smoke。Expected: 序列内容一致，Python 可打开并读取 store。

- [ ] **Step 6: 最终静态检查**

```bash
find src -maxdepth 1 -type f -printf '%f\n' | sort
rg -n 'ProteinAAStore.hpp|ProteinAAStore::|build_from_store' src CMakeLists.txt README.md
git diff --check
```

Expected: `src/` 仅包含设计列出的 7 个文件；旧符号扫描无匹配；diff check 通过。
