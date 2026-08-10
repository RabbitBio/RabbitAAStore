# AAStore Byte Range

这是从原版 `PagedProteinAAStore` 收缩出的可建库、可读取实现。磁盘格式仍是原版：

- `meta.bin`：`AAPCMP2` compact sequence offsets
- `pages.bin`：连续的 5-bit packed 4 KiB pages
- `seq_names.txt`：FASTA 名称

读取端只有一条数据路径：sequence ID 查 residue offset，按 ID/物理 offset 排序，将相近的
packed extents 合并，`pread` 后用原版 SIMD kernel 解码，再 scatter 回请求顺序。默认行为
仍是固定 `4096` bytes merge gap；也可启用自适应 planner。没有 AAStore 自己管理的 page
cache、LRU、page planner、streaming 或第二套磁盘格式。

自适应 planner 对 batch 的 gap 分布抽样，以 `I/O latency + bytes / bandwidth` 比较 14 个
候选 gap。Linux 下再均匀抽样 64 个文件页的内核 page-cache 驻留状态，在冷热模型之间
插值；其他平台回退到热态模型。合并后的单个 range 默认限制为 1 MiB，避免大 range
buffer 抬高 RSS。

## 文件职责

```text
include/aastore/store.hpp      公共建库和 batch_get API
src/builder.cpp                FASTA -> 原版 meta.bin/pages.bin
src/metadata.cpp               AAPCMP1/2、AAPAGE3/4 元数据兼容
src/codec.cpp                  原版 stop-aware 5-bit 编解码和 SIMD dispatch
src/byte_range_planner.cpp     radix sort + fixed/adaptive byte-range 合并
src/store.cpp                  并行 range read、解码和 scatter
```

## 构建

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

## 使用

```cpp
#include <aastore/store.hpp>

aastore::Builder::build_fasta("proteins.fa", "proteins.aastore");

aastore::Store store("proteins.aastore");
aastore::ReadOptions options;
options.io_threads = 8;
options.adaptive_merge_gap = true;
auto batch = store.batch_get({9, 2, 100, 2}, options);
for (size_t i = 0; i < batch.size(); ++i) {
    consume(batch.view(i));
}
```

不设置 `adaptive_merge_gap` 时保持固定 4 KiB 行为，也可通过
`options.merge_gap_bytes` 设置固定 gap。自适应模式常用入口只有：

```cpp
aastore::ReadOptions options;
options.io_threads = 8;
options.adaptive_merge_gap = true;
auto batch = store.batch_get(ids, options);
```

`options.adaptive.io_latency_ns`、冷热 bandwidth 和
`options.adaptive.max_merged_range_bytes` 暴露给基准校准；通常无需逐项设置。

## 基准

```bash
./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --adaptive-gap

./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --adaptive-gap --drop-cache
```

`--drop-cache` 使用 `POSIX_FADV_DONTNEED` 并在计时前等待 1 秒。它是 page-cache
冷启动提示，不等价于清空设备或远端存储缓存。
