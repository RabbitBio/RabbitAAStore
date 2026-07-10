# AAStore 源码目录收敛设计

## 目标

让 `src/` 只保留当前 paged AAStore 的生产代码和编译依赖，彻底删除旧
`ProteinAAStore.hpp`，其余工具按用途归档到独立目录。

## 生产目录

整理后 `src/` 只保留：

```text
PagedProteinAAStore.hpp
PagedAAStoreTools.cpp
Packed5BitEncoder.hpp
Packed5BitDecoder.hpp
aastore_c_api.cpp
CLI11.hpp
kseq.h
```

## 删除旧依赖

- 删除 `src/ProteinAAStore.hpp`；
- 删除 `PagedProteinAAStore::build_from_store()`；
- 删除 `paged_aatools build` 旧二进制转换子命令；
- 将 `ProteinAAStore::enc(c)` 的低 5-bit 映射移动到
  `Packed5BitEncoder.hpp`，由 paged builder 直接调用；
- 保留 `build-fa`，新 store 统一直接从 FASTA 构建。

这会有意删除“旧 ProteinAAStore 二进制转 paged store”的兼容入口，不改变
现有 paged store 格式、`build-fa`、open/get/fetch 或 C/Python API。

## 归档布局

```text
tests/
  paged_batch_regression.cpp
  paged_prefetch_verify.cpp

benchmarks/
  homolog_fetch_aastore_bench.cpp
  kseq_benchmark.cpp
  meta_index_benchmark.cpp
  rabbityclust_store_kernel_bench.cpp

legacy/
  AAStoreTools.cpp
  benchmark.cpp
  kmercount.cpp
  rabbityclust_old_store_build.cpp
  rabbityclust_old_store_build_fast.cpp

experiments/page_hot_density/
  PageHotDensityMatrix.hpp
  page_hot_density_matrix_regression.cpp
```

归档文件不参与默认构建。依赖已经删除的旧实现允许保留为历史源码，但不会
承诺可单独编译。

## CMake

默认仅产生：

```text
paged_aatools
libaastore_c.so
```

增加 `AASTORE_BUILD_TESTS=OFF`。启用后从 `tests/` 构建
`paged_batch_regression`；io_uring 与 include/link 设置沿用现有规则。

旧 `aatools`、`kmercount`、通用 benchmark 和归档工具目标全部从默认 CMake
移除。

## 验证

1. 默认全新 CMake 构建只包含 `paged_aatools` 和 `aastore_c`；
2. `AASTORE_BUILD_TESTS=ON` 时 regression 构建并通过；
3. `paged_aatools build-fa` 构建小 FASTA，随后 `get` 返回正确序列；
4. Python C API smoke 通过；
5. `src/` 和 CMake 中没有 `ProteinAAStore.hpp`、`ProteinAAStore::` 或
   `build_from_store`；
6. `paged_aatools --help` 不再公开旧 `build` 子命令。

## 非目标

- 不重写归档的旧工具；
- 不改变 paged 文件格式；
- 不继续执行已暂停的 page-hot density matrix；
- 不清理仓库根目录的实验结果文件。
