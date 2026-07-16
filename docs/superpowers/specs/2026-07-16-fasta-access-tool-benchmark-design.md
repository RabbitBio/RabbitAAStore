# AAStore 与 FASTA 访问工具比较实验设计

## 目标

按照 pyfastx、pyfaidx 和 SeqKit 论文中的主要实验方法，比较 AAStore、
pyfastx、pyfaidx、samtools faidx 和 SeqKit 在大型蛋白质 FASTA 数据上的：

- 索引或存储构建时间、峰值 RSS 和磁盘占用；
- 完整序列随机回取时间和峰值 RSS；
- 全库顺序遍历吞吐与峰值 RSS；
- 按 ID 列表批量提取 FASTA 的端到端性能。

实验同时保留“论文复现口径”和“严格等价口径”。前者便于直接说明实验方法与
已有论文一致，后者增加线程、缓存和输出正确性控制，作为 AAStore 的主要结果。

## 依据

pyfastx 论文使用 15 个 12 MB 至 32 GB 的 FASTA 数据集，对索引构建、随机回取
30% 序列、随机回取 1000 个 1 Kb 区间和顺序遍历进行比较。每项运行三次，使用
`/usr/bin/time -f "%e %M"` 记录 wall time 和峰值 RSS，再取平均值。另以 106 GB、
超过 3.05 亿条序列的 UniParc 仅做索引构建压力测试。

pyfaidx 论文重点比较索引初始化和随机 1000 bp 区间访问，并以三次重复报告时间
与内存。SeqKit 论文分别测试顺序解析、ID 列表搜索、采样、去重和区间提取，运行
三次或五次后报告平均时间和内存。

本实验不照搬与蛋白质整序列存储不适配的固定长度核酸区间测试。AAStore 不提供
任意坐标区间访问，因此主比较采用所有工具共同支持的完整蛋白质序列回取。

参考实现与论文：

- https://academic.oup.com/bib/article/22/4/bbaa368/6042388
- https://github.com/lmdu/pyfastx/tree/master/benchmark
- https://peerj.com/preprints/970v1/
- https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0163962

## 数据规模

使用同源合成蛋白质 FASTA，记录名和序列在各规模之间保持相同生成规则：

1. 1M 数据仅用于脚本回归和正确性冒烟，不进入主图。
2. 10M 数据运行完整矩阵，是主要比较数据集。
3. 100M 数据运行索引构建压力测试，以及 100K 和 1M 完整序列随机回取。

现有数据路径为：

```text
/home/bigssd/test_cdhit/1m_nr.fa
/home/bigssd/test_cdhit/10m_nr.fa
/home/bigssd/test_cdhit/100m_nr.fa
```

每个数据集记录 FASTA 文件字节数、记录数、总 residues、平均长度、最长长度和
SHA-256。正式结果中保留数据生成方法，不把现有索引视为构建阶段输入。

## 工具与版本

比较以下五种后端：

1. AAStore，使用当前仓库构建的固定 commit。
2. pyfastx，使用隔离 Python 环境中的固定版本。
3. pyfaidx，使用隔离 Python 环境中的固定版本。
4. samtools faidx，固定可执行文件和版本。
5. SeqKit，下载固定版本的官方 Linux 二进制并记录 SHA-256。

运行前生成 `environment.json`，记录 git commit、dirty 状态、工具版本、Python
版本、内核、CPU、内存、文件系统、命令行和线程环境。SeqKit 主比较固定为单线程；
另可报告其默认或多线程结果，但不得与单线程主结果混合。

## 请求清单

从 FASTA 记录号空间使用固定种子 `123` 无放回抽样，同时生成：

- `request_ids.u64`：AAStore 的零基数值 ID；
- `request_names.txt`：其他工具使用的第一字段序列名；
- `request_manifest.tsv`：数值 ID、名称、长度和源文件顺序。

清单生成不计入回取时间。名称和数值 ID 的一一对应关系在正式运行前验证。10M
数据生成 1K、10K、100K、1M 和 30% 五种规模；100M 数据生成 100K 和 1M 两种
规模。主清单保持随机请求顺序且不含重复项。

SeqKit 和部分命令行模式不能可靠保持任意请求顺序，因此另外生成按源文件顺序
排序的等价清单，专用于所有五种工具共同参与的规范 FASTA 输出比较。随机顺序
清单用于支持任意 name/ID 随机访问的后端，并单独标明任务语义。

## 实验矩阵

### 索引与存储构建

从没有索引的 FASTA 副本或链接开始，分别构建：

- AAStore pages、meta 和可选 name index；
- pyfastx `.fxi`；
- pyfaidx `.fai`；
- samtools `.fai`；
- SeqKit 自身 `faidx` 所需索引。

记录总 wall time、user/system CPU、峰值 RSS、退出状态、索引总字节数和构建后的
可查询记录数。AAStore 分开报告纯数值 ID 存储和带名称索引的占用，主 name-based
比较使用后者。

### 完整序列随机回取

论文复现口径使用固定名称清单，逐个回取完整序列并将输出写到 `/dev/null`。
pyfastx 和 pyfaidx 使用最小 Python 驱动；samtools 使用 `faidx -r`；SeqKit 使用
`faidx --infile-list`；AAStore 使用名称索引解析加批量 fetch。

严格等价口径要求每个后端：

- 接收对应的同一请求清单；
- 实际读取并生成所有 residues；
- 以统一规则生成规范化 FASTA 或流式更新相同摘要；
- 保留请求顺序的后端使用随机顺序清单；
- 全工具公共结果使用源文件顺序清单；
- 名称解析、序列读取和输出均计入端到端 wall time。

AAStore 额外报告 `0 MiB`、`128 MiB` 和全内存三种模式，但以 `0 MiB` 与
`128 MiB` 为主要低内存结果。不能把 AAStore 数值 ID 结果与其他工具 name 查找
结果直接放在同一主表；数值 ID 路径作为独立上限结果。

### 顺序遍历

所有工具顺序读取全部记录，累计记录数、residue 数和流式摘要，不保存所有序列。
AAStore 通过递增数值 ID 遍历。该实验说明流式解析能力，不作为 AAStore 随机批量
读取的核心优势结论。

### ID 列表批量提取

使用按源文件顺序排列的唯一名称清单，让所有工具输出统一格式 FASTA：header 仅
保留主名称，序列单行输出。输出写入临时文件，计入完整写出时间；完成后比较记录
数、residue 数和 SHA-256。另提供输出到 `/dev/null` 的纯读取结果，二者分表报告。

## 缓存、线程与重复

每个主要实验包含：

- warm：不主动回收文件缓存，先预热一次后测量；
- cold：在独立进程启动前对相关数据与索引执行 `POSIX_FADV_DONTNEED`，并记录
  是否成功；不能宣称为系统全局冷缓存；
- 三次正式重复，报告论文一致的平均值，同时报告中位数、最小值、最大值和标准差；
- 后端顺序按重复轮次轮换，降低固定运行顺序偏差；
- 主结果固定一个 CPU core 和一个工具线程，多线程扩展单独报告。

每个进程通过 GNU time 和 `/proc` 采集 wall time、user/system CPU、最大 RSS、
主/次缺页、读写字节和退出码。超时或 OOM 保留为结果，不静默丢弃。

## 正确性

正式计时前，1M 数据上的所有后端必须通过：

- 名称到记录的一一映射；
- 1K 随机记录的长度和 residue 逐条一致；
- 随机清单中首、中、末记录顺序一致；
- 规范 FASTA 的记录数、总 residues 和 SHA-256 一致；
- 不存在名称时返回非零状态或可识别错误。

正式运行后再次验证每个结果摘要。任何 checksum 不一致的性能数据标记为无效，
不得进入速度排名。

## 产物

实验代码放入独立 benchmark 目录，不修改 AAStore 存储格式。输出包括：

- 可重复执行的安装、索引、请求生成和 benchmark 脚本；
- 每次运行的原始 JSONL/TSV 与 stderr；
- 汇总 CSV/JSON；
- 时间、RSS、索引空间和吞吐图；
- 方法、版本、失败项、正确性和解释边界说明。

正式脚本必须支持单独运行某一数据规模、任务、后端、缓存状态或重复轮次，也必须
能够断点续跑而不覆盖已有原始结果。

## 临时文件清理

规范 FASTA 输出、GNU time 临时文件、子进程 stderr 临时文件和 `.part` 文件仅在
单次运行期间存在。运行成功时，先把输出字节数、记录数、residue 数和 SHA-256
原子写入结果 JSON，再删除大体积 FASTA 输出；运行失败或收到中断时也通过清理
逻辑删除本次未完成的临时输出。

正式实验保留输入数据、构建索引或 AAStore、请求清单、环境记录、原始结果 JSON、
必要的失败日志和汇总报告。清理过程不得删除这些复现材料，也不得删除其他实验
目录中的既有文件。

## 非目标

- 不比较 FASTQ、gzip FASTA 或任意核酸坐标区间。
- 不把多线程 SeqKit 与单线程 Python 工具混为同一公平结果。
- 不把已有索引的首次打开时间称为索引构建时间。
- 不因某工具不支持任意请求顺序而在其外部增加未计时排序。
- 不只比较内部 fetch 时间而省略名称解析或实际输出生成。
