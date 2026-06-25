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
- `build/paged_aatools`：构建/测试半内存半磁盘的 paged 5-bit 库

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

## PagedProteinAAStore
- `pages.bin`：顺序存储 5-bit packed pages
- `meta.bin`：存储头信息和 `start_positions`，不再保存 `SeqMeta` / `PageMeta`
- page 偏移：固定 packed page 大小时用 `page_id * packed_page_bytes` 计算；最后一页长度由 `total_length % page_residues` 计算
- 序列定位：`start = start_positions[id]`，`length = start_positions[id + 1] - start`，再用 `start / page_residues` 和 `start % page_residues` 得到 page 内位置
- `PageCache`：`--preload-cache` 使用连续内存的 direct-prefix cache，命中判断为 `page_id < preload_pages`；每个 page 在内存中按固定 stride 对齐，非预加载 miss 保留小型 LRU fallback
- packed page 大小固定为 `4096` bytes，对应 `page_residues = 6553`
- `build-fa -j/--threads`：FASTA 构建时用于 page packing 的 worker 线程数，默认 `1`；设为 `0` 使用硬件线程数
- `build-fa --names-output`：批量缓冲保存 kept 序列名，一行一个 name，顺序和 `seq_id` 一致；默认写到输出目录的 `seq_names.txt`
- FASTA 读取：项目内 `kseq` buffer 调整为 `1MiB`，减少大 FASTA 顺序读取时的 `fread` 次数
- `--memory-mb`：常驻 page cache 内存预算；若能装下整个 `pages.bin`，自动 full raw cache 并走普通随机 get。`--cache-mb` 是旧别名
- `--prefetch-mb`：半内存半磁盘时的临时 miss-page 预取缓冲区大小；不会决定 full raw cache。设为 `0` 则关闭预取并走普通随机 get
- `--preload-cache`：手动预加载 PageCache；当 `--memory-mb` 足够装下 `pages.bin` 时会自动 full preload，不需要显式指定
- `--preload-threads`：preload 使用的读取线程数；默认 `0`，自动选择最多 `16` 个线程
- `--prefetch-threads`：batch prefetch 的总 I/O 执行流数量，包含调用线程，默认 `2`。`1` 或 `0` 表示只使用调用线程的一个异步 ring；`2` 表示调用线程加一个后台 I/O 线程
- `prefetch_miss_pages` / `prefetch_ranges` / `prefetch_read_bytes`：benchmark 输出的 miss-page 预取统计；当前不做 range 合并，所以 `prefetch_ranges` 等于实际预取 page 读取次数
- `build` / `build-fa`：构建时在标准错误输出进度条；`build-fa` 直接使用 kseq 的序列 buffer 写 packed page，避免每条序列复制成 `std::string`，也避免先收集 `page_codes` 再重打包；`build-fa` 按 FASTA 字节显示进度条，并每读 10000 条序列刷新一次 `read_seqs/kept_seqs`，`build` 按序列数显示

## 使用示例
```bash
# 从 FASTA 构建并保存
./build/aatools -f /path/input.fa -o /tmp/store_dir

# 从二进制库加载
./build/aatools -l /tmp/store_dir

# k-mer 统计（k=7，限制内存 64GB，输出到文件）
./build/kmercount -k 7 -i /tmp/store_dir/protein_store.bin -M 65536 -o /tmp/kmer.tsv

# 从现有 ProteinAAStore 二进制构建 paged store
./build/paged_aatools build -i /tmp/legacy_store.bin -o /tmp/paged_store

# 直接从 FASTA 流式构建 paged store（不需要先加载完整库）
./build/paged_aatools build-fa -f /path/input.fa -o /tmp/paged_store -j 8

# 半内存半磁盘随机访问：memory-mb 装不下 pages.bin，prefetch-mb 作为临时 miss-page 缓冲
./build/paged_aatools bench -i /tmp/paged_store -n 10000 --memory-mb 8192 --prefetch-mb 512

# 全量加载测试：memory-mb 给到足够装下 pages.bin，prefetch-mb 可以保持默认或设为 0
./build/paged_aatools bench -i /tmp/paged_store -n 100000 --memory-mb 32768 --prefetch-mb 0 --preload-threads 8
```
