# 使用说明

## 构建
```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```
> GCC 8 requires linking `stdc++fs`, handled by CMakeLists.txt.

## 可执行文件
- `build/aatools`：构建/加载 `ProteinAAStore` 二进制库
- `build/kmercount`：对 `ProteinAAStore` 做 k-mer 频次统计

## aatools 参数
- `-f, --fasta <file>`：输入 FASTA（与 `--load` 互斥）
- `-l, --load <file|dir>`：从二进制库加载；若为目录，默认文件名 `protein_store.bin`
- `-o, --output <file|dir>`：保存二进制库；若为目录，默认文件名 `protein_store.bin`
- `-n, --names-output <file>`：保存序列名（不含 `>`），默认在输出目录生成 `protein_names.txt`
- `--names-index-output <file>`：保存序列名索引（uint64_t 偏移），默认 `protein_names.txt.idx`
- `--names-load <file>`：加载序列名文件（可选）
- `--names-index-load <file>`：加载序列名索引文件（可选，默认 `names + .idx`）
- `-L, --min-length <uint>`：最短序列长度过滤（短于该值将跳过，默认 0 不过滤）

## kmercount 参数
- `-k, --kmer <int>`：k-mer 长度（1–12）
- `-i, --input <file>`：输入 `ProteinAAStore` 二进制文件（必填）
- `-o, --output <file>`：输出文件（默认标准输出）
- `-M, --memory <MB>`：最大内存（MB，0 表示自动检测可用内存的 80%）
- `-m, --min-count <uint>`：最小保留频次（默认按序列数自动计算）
- `-t, --top <int>`：只输出频次最高的前 N 个（默认 0=全部）
- `-c, --cutoff <uint>`：高频 k-mer 频次阈值（默认 0=自动检测）
- `-p, --percentile <float>`：自动检测阈值的分位数（默认 0.95）
- `-H, --histogram / --no-histogram`：输出/关闭频次直方图（默认开启）
- `--mode <array|hashmap|multipass>`：强制存储模式（默认自动选择）
- `-j, --threads <int>`：线程数（默认 CPU 核心数）

## 存储格式（`ProteinAAStore`）
- 5-bit 编码序列：`data_`（`sdsl::int_vector<5>`）
- 起始位置数组：`start_positions_`（每条序列起点，末尾有哨兵，长度 `n_ + 1`）
- 序列数：`n_`
- `save()` 写入顺序：`n_` → `start_positions_` size+data → `data_.serialize`
- `load()` 按相同顺序读取，并校验 `start_positions_.size() == n_ + 1`
- 序列名：`names_` 可选；`save_names()`/`load_names()` 使用独立文件和索引文件

## ProteinAAStore 主要能力
- `add(seq)` / `add(seq, name)` / `finalize()`：增量构建（使用 `init(size)` 预留容量以减少 `resize()` 开销）
- `get(i)` / `get_raw(i, out)`：获取序列（字符串或 5-bit 原始编码）
- `length(i)` / `checksum(i)` / `total_length()`：长度与轻量校验
- `memory_usage()` / `memory_breakdown()`：内存估算
- `save(path)` / `load(path)`：二进制存取
- `save_names()` / `load_names()`：序列名和索引文件的保存/加载

## 使用示例
```bash
# 从 FASTA 构建并保存
./build/aatools -f /path/input.fa -o /tmp/store_dir

# 从二进制库加载
./build/aatools -l /tmp/store_dir

# k-mer 统计（k=7，限制内存 64GB，输出到文件）
./build/kmercount -k 7 -i /tmp/store_dir/protein_store.bin -M 65536 -o /tmp/kmer.tsv
```
