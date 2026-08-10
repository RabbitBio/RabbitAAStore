# AAStore Byte Range

这是从原版 `PagedProteinAAStore` 收缩出的可建库、可读取实现。磁盘格式仍是原版：

- `meta.bin`：`AAPCMP2` compact sequence offsets
- `pages.bin`：连续的 5-bit packed 4 KiB pages
- `seq_names.txt`：FASTA 名称

读取端只有一条路径：sequence ID 查 residue offset，按 ID/物理 offset 排序，将相距不超过
固定 `4096` bytes 的 packed extents 合并，`pread` 后用原版 SIMD kernel 解码，再 scatter
回请求顺序。没有 page cache、LRU、page planner、streaming 或第二套磁盘格式。

## 文件职责

```text
include/aastore/store.hpp      公共建库和 batch_get API
src/builder.cpp                FASTA -> 原版 meta.bin/pages.bin
src/metadata.cpp               AAPCMP1/2、AAPAGE3/4 元数据兼容
src/codec.cpp                  原版 stop-aware 5-bit 编解码和 SIMD dispatch
src/byte_range_planner.cpp     radix sort + 固定 4 KiB gap 合并
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
auto batch = store.batch_get({9, 2, 100, 2}, options);
for (size_t i = 0; i < batch.size(); ++i) {
    consume(batch.view(i));
}
```

`kByteRangeMergeGapBytes` 固定为 `4096`，不再提供运行时调参入口。

## 基准

```bash
./build/aastore_bench /path/to/original/store \
  --requests 1000000 --threads 8 --drop-cache
```
