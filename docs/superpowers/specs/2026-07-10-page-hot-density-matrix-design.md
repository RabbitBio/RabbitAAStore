# Page-hot 请求密度合成矩阵设计

## 目标

在不修改 AAStore 正式缓存路径的前提下，离线验证“请求加权 page
需求密度”是否能稳定识别热点，并量化取消 admission budget 后可能产生的
换入换出规模。

本实验回答三个问题：

1. 50% 密度阈值在不同数据库规模和访问分布下会选出多少 page；
2. 经过 2Q 二次确认后，会有多少 page 真正晋升；
3. 晋升 page 在后续批次能覆盖多少请求与残基需求。

## 非目标

- 不修改 `PagedProteinAAStore` 的正式 admission 行为；
- 不读取或解码 `pages.bin`；
- 不评价真实 I/O wall time；
- 不在本实验中加入 admission budget。

## 热度定义

对一个批次中的每个 page，计算请求加权需求量：

```text
demand_residues(page) = sum(chunk.length)
density(page) = demand_residues(page) / residues_in_page(page)
```

重复 sequence ID 产生的 chunk 重复累计，因此 `density` 可以超过 100%。
尾页使用实际 `residues_in_page(page)` 作为分母。

实验同时统计 25%、50%、75% 和 100% 四档阈值。拟采用的默认规则是：

```text
density >= 50%
并且最近 8 批中第二次达到该阈值
=> 2Q promotion candidate
```

合成矩阵只有 4 轮，但分析器保留 8 轮 epoch 语义。

## 数据集

使用已有 store，不重新构建：

| 数据集 | Store | 每轮请求数 |
|---|---|---:|
| 10M | `/home/user_home/gaoyijie/build_eval_current/10m_store` | 1,000,000 |
| 100M | `/home/user_home/gaoyijie/build_eval_current/100m_store` | 10,000,000 |

两组都抽取数据库序列数的 10%，便于比较规模效应。分析只打开 metadata，
不读取 `pages.bin` payload。

## 合成访问分布

每个 workload 运行 4 个批次，使用固定 seed，保证可复现。

### Uniform Fresh

每轮独立均匀随机抽样。用于验证没有稳定热点时的误判率。

### Uniform Repeat

四轮重复完全相同的均匀随机请求。用于验证广泛重复扫描是否会导致大量
page 同时通过 2Q。

### Persistent 80/20 Hotspot

80% 请求来自固定的 1% sequence ID 热点池，20% 来自全库均匀随机。
热点池通过 SplitMix64 分散到全库 ID 空间，避免只集中在物理 prefix。

### Phase Shift

前两轮使用热点池 A，后两轮切换到互不重叠的热点池 B。用于观察 8 轮窗口
是否保留旧热点，以及新热点何时产生第二次确认。

## 分析器数据流

1. 用 `PagedProteinAAStore` 只打开 metadata；
2. 按 workload 流式生成 sequence ID，不保留完整 ID 数组；
3. 通过 `sequence_start_unchecked()` 和 `sequence_length_unchecked()` 把请求切成
   page overlap；
4. 在 dense `uint64_t` demand 数组中累计残基需求，并记录本批 touched pages；
5. 对 touched pages 计算 density、阈值候选和旧 refs 阈值候选；
6. 更新每个阈值的最近候选 epoch，模拟 2Q first miss 与 promotion；
7. 下一轮统计已 promotion page 覆盖的 page requests 和 demand residues；
8. 每轮仅清零 touched page，避免扫描全部 page。

100M 分析器不保存 10M 个请求 ID；主要额外内存来自按 page 分配的 demand、
reference count 和 epoch 数组。

## 对照与指标

除 density 25/50/75/100% 外，同时统计旧引用阈值：

```text
refs >= 2 / 8 / 32
```

每轮输出：

- request 数、request residues；
- touched page 数；
- density 分布桶：`<25`、`25-50`、`50-75`、`75-100`、`100-200`、`>=200`；
- 各 density 阈值 candidate 数；
- 各 refs 阈值 candidate 数；
- 2Q first misses、promotions；
- promotion page 数与估算替换字节；
- 已 promotion page 对下一轮 page requests 和 demand residues 的覆盖率；
- 分析 wall time 与峰值 RSS，写入独立 `.time.txt` sidecar，不进入要求
  字节可复现的主结果文件。

## 输出

结果写入当前仓库：

```text
page_hot_density_matrix_20260710/
  10m_uniform_fresh.txt
  10m_uniform_fresh.time.txt
  10m_uniform_repeat.txt
  10m_hot80_20.txt
  10m_phase_shift.txt
  100m_uniform_fresh.txt
  100m_uniform_repeat.txt
  100m_hot80_20.txt
  100m_phase_shift.txt
  100m_phase_shift.time.txt
  summary.tsv
  README.md
```

`README.md` 只根据实际结果下结论，不预设 50% 一定优于固定 refs。

## 验证要求

- 同一 seed 重跑必须产生完全相同的 candidate/promotion 数；
- 所有 density 阈值 candidate 数必须单调不增；
- `Uniform Fresh` 的跨批 promotion 应显著低于稳定热点 workload；
- `Phase Shift` 第三轮不能立即把新热点计为二次确认；
- 10M 小样本结果需用独立朴素实现抽查；
- 分析器不得修改 store，也不得产生 `pages.bin` I/O。

## 决策门槛

完成矩阵后再决定正式策略：

- 如果 50% 在稳定热点下仍几乎没有候选，则下调到 25%；
- 如果 `Uniform Repeat` 在 50% 下产生大规模 promotion，则正式实现需加入
  admission budget 或更强的相对热度门槛；
- 如果 50% 在 80/20 和 phase-shift 中能以较小 promotion 集合覆盖大部分后续
  demand residues，则进入正式 `page_hot_lru` 实现。
