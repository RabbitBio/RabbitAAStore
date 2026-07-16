# HMMER esl-sfetch 接入 AAStore 设计

## 目标

为 Easel 的 `esl-sfetch` 增加可选 AAStore 后端。在不改变原有 FASTA/SSI
后端的前提下，让一批蛋白质名称通过 AAStore 名称索引解析为 sequence ID，
随后用一次批量请求从磁盘或受限内存缓存中回取完整序列，并继续通过标准输出
交给 `hmmsearch` 等程序。

首轮实验只验证完整蛋白质序列回取，不扩展坐标区间、反向互补或数据库构建功能。

## 方案选择

考虑过三种方案：

1. 独立实现一个 AAStore 取序工具。改动最小，但不能证明 HMMER/Easel 已经接入。
2. 在 `esl-sfetch` 中增加可选 AAStore 后端。保留现有命令行和输出流程，改动集中，
   也能直接进入 `esl-sfetch | hmmsearch` 管道。
3. 将 AAStore 实现为通用 `ESL_SQFILE` 后端。可以覆盖更多 HMMER 命令，
   但需要侵入 Easel 序列 I/O 抽象，首轮风险和验证范围都过大。

采用方案 2。它是真实的软件接入，同时仍能与未修改的 `esl-sfetch` 做公平对比。

## 用户接口

原始命令保持不变：

```bash
esl-sfetch -f proteins.fa ids.txt
```

AAStore 后端使用：

```bash
esl-sfetch --aastore -f /path/to/store ids.txt
```

单条完整序列也支持：

```bash
esl-sfetch --aastore /path/to/store SEQUENCE_NAME
```

AAStore 参数使用独立选项，避免影响原后端：

- `--aastore-memory-mb`：`-1` 为全内存，`0` 为无内存，正数为半内存。
- `--aastore-prefetch-mb`：批量预取总内存，默认 128 MiB。
- `--aastore-names`：覆盖默认的 `STORE/seq_names.txt`。

AAStore 模式仅接受名称列表和完整序列回取。坐标区间、反向互补等尚未实现的组合
直接返回非零退出码和明确错误信息。

## 数据流

批量模式的数据流为：

```text
ids.txt
  -> 保序读取名称
  -> AAStore 名称索引批量解析 sequence ID
  -> 批量计算 offsets/lengths
  -> 分配单个连续 arena
  -> aastore_batch_get_arena_prepared
  -> 按原请求顺序写出 FASTA
  -> stdout / hmmsearch
```

输出阶段不为每条序列创建独立字符串。FASTA header 使用 AAStore 保存的主名称，
序列直接从 arena 分段写出。原始 FASTA 描述字段没有保存在 AAStore 中，因此不要求
与 SSI `Echo()` 路径字节完全一致；名称、请求顺序和 residues 必须一致。这样 AAStore
的主要对比对象仍是索引定位、磁盘读取和解码，而不是大量小对象分配。

单条模式复用同一条路径，只是 batch 大小为 1。

## 代码边界

AAStore 仓库新增稳定的 C API 声明头文件，并让现有 `aastore_c_api.cpp` 使用该声明。
HMMER 实验源码只在 `esl-sfetch` 增加：

- AAStore 命令行选项；
- 名称列表解析与错误定位；
- 一次批量 arena fetch；
- FASTA 流式输出。

不修改 AAStore page/meta 格式，不修改 HMMER 搜索算法，也不替换 HMMER 的普通
FASTA/SSI 路径。

## 错误处理

- 名称不存在时，报告名称及其在输入列表中的行号，并终止本批请求。
- AAStore 打开、layout 或 fetch 失败时，输出 `aastore_last_error()`。
- arena 大小溢出、分配失败和输出写失败都返回非零退出码。
- 输入名称顺序必须保留。与原版 `esl-sfetch -f` 一致，重复名称直接报错。

## 验证与基准

正确性测试覆盖：

- 单条名称回取；
- 多条名称保序回取；
- 重复名称报错；
- 不存在名称；
- AAStore 输出的主名称、顺序和 residues 与原 `esl-sfetch` 逐条一致。

性能测试使用同一 FASTA 来源、同一固定名称列表、独立进程和冷文件缓存，比较：

1. 原版 `esl-sfetch` + SSI；
2. AAStore `esl-sfetch` 无内存模式；
3. AAStore `esl-sfetch` 半内存模式。

分别记录 open、名称解析、layout、fetch、输出、总时间和峰值 RSS。主测试先使用
100 万条蛋白质库完成回归，再在 1000 万条蛋白质库上测试 1 万、10 万和 100 万个
随机名称。输出写到 `/dev/null`，但完整序列仍必须实际生成并写出。

最后增加真实管道冒烟：

```bash
esl-sfetch --aastore -f STORE candidate_ids.txt | hmmsearch model.hmm -
```

验证 HMMER 能读取 AAStore 后端生成的 FASTA，并比较搜索结果一致性。

## 非目标

- 不把 AAStore 接入 `jackhmmer` 的全库扫描。
- 不修改 HMMER profile、alignment 或统计计算。
- 不在首轮实现 accession alias、坐标区间和反向互补。
- 不用该实验改动现有 MMseqs2 接入。
