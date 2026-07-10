# Page-hot LRU 默认策略设计

## 目标

删除大批次 LRU admission freeze 和普通 LRU 模式。半内存只保留 `prefix` 与 `page_hot_lru` 两种 canonical 策略。`page_hot_lru` 使用带页热度门槛的 2Q-lite：只有当前批次中请求密度足够高的 page 才进入 2Q admission，避免宽随机批次把 resident cache 冲掉，同时允许真正热点 page 被逐步替换进内存。

## 默认行为

- 默认阈值保持 `page_hot_lru_min_refs = 32`。
- `ResidentCachePolicy::Lru` 改为 canonical 的 `ResidentCachePolicy::PageHotLru`。
- LRU page 必须同时满足以下条件才能 admission：
  1. 当前批次对该 page 的引用数不少于 32；
  2. 通过现有 2Q-lite 的二次访问 admission 判断。
- `prefix` 策略不执行 admission，行为不变。
- no-memory 和 full-memory 没有可替换的 partial resident LRU，行为不变。

## 删除 freeze

删除以下运行时机制：

- `lru_freeze_coverage_threshold` 配置；
- `set_lru_freeze_coverage_threshold()`；
- `lru_admissions_frozen_` 状态；
- `should_freeze_lru_admissions_for_batch()`；
- `lru_admission_frozen_batches/pages` 统计及 CLI 输出；
- range builder 中的 `freeze_lru_admissions` 判断。

删除后，page-hot filter 成为控制宽批次 admission 的唯一批内门槛，2Q-lite 继续控制跨批次的二次访问。运行时不再存在关闭 page-hot filter 的普通 LRU 分支。

## CLI

- `--cache-policy` 只公开 `prefix` 和 `page_hot_lru`。
- 旧名称 `lru`、`lru_2q_lite`、`2q_lite` 作为兼容别名解析到 `page_hot_lru`。
- 策略名称输出统一报告 `page_hot_lru`，即使输入使用旧别名。
- 删除 `--page-hot-lru` / `--no-page-hot-lru` 开关。
- `--page-hot-lru-min-refs N` 保持可配置，默认 32。

## 测试

1. 默认 resident cache policy 必须为 `page_hot_lru`，阈值为 32。
2. `lru`、`lru_2q_lite`、`2q_lite` 必须解析为 `page_hot_lru`，canonical 名称必须输出 `page_hot_lru`。
3. 高阈值下，低密度 miss page 必须被 filter，不能产生 admission 或 eviction。
4. 满足热度阈值的 page 仍必须进入现有 2Q-lite 判断，并能在重复访问后 admission。
5. `prefix`、no-memory、full-memory 和序列结果正确性回归必须通过。
6. 在 10M store、1M 固定随机请求、1072 MiB resident cache 下复测 `page_hot_lru`；确认不再存在 freeze 统计，且结果 checksum 与 prefix/no-memory 一致。

## 性能预期

宽随机批次中，大多数 page 的请求数远低于 32，因此 admission/eviction 接近零；与旧 freeze 行为相比，主要差异是保留少数真正高密度 page 的学习机会。该修改不直接减少 sort/chunk/group 成本，预期整体性能接近当前 prefix/LRU freeze 基线。
