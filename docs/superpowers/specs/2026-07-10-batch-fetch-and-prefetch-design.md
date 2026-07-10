# AAStore 批量取数与预取优化设计

## 目标

降低全内存模式的输出准备成本，以及无内存、半内存模式的页面 I/O 开销，同时保证解码后的序列内容、请求顺序、缓存容量和用户传入输出缓冲区的语义不变。

## 修改范围

这次修改覆盖 `PagedProteinAAStore` 中相互关联的四部分：

1. 优化 `FetchResult` 的输出准备。
2. 使用统一的批处理线程数控制常驻页面解码和 miss I/O。
3. 按 page 优先展开重复的序列 ID。
4. 有上限地合并相邻的缺失页面。

这次不修改 HotSeq overlay 策略、磁盘物理页面布局、编码方式、元数据格式和构建流水线。

## FetchResult 输出优化

`FetchResult::storage` 改用 `AANoInitAllocator<char>`。扩展输出 arena 时不再先把即将被解码结果完全覆盖的内存清零。

`offsets` 继续按照用户传入的请求顺序描述每条序列的位置。

为兼容现有源码，暂时保留 `FetchResult::output_ptrs` 字段，但 `fetch()` 和 `fetch_into()` 不再填充它。内部直接通过 `ArenaOutputSource { storage.data(), offsets.data() }` 定位每条序列的输出地址。

非空序列的最后一个 chunk 解码完成时，直接在序列末尾写入 `\0`。空序列在准备 arena 时写入终止符。这样可以删除解码完成后的 `terminate_fetch_result()` 全量扫描。

返回的 view 和 string 内容必须与修改前逐字节一致。重复使用同一个 `FetchResult` 时，vector 的 capacity 可以跨批次保留。

## 统一线程数

`Options::io_threads` 作为统一的批处理线程数，同时控制：

- 异步 miss I/O 的并发流数量；
- 常驻 cache hit 页面的并行解码；
- full raw cache 的并行解码。

`configure()` 使用这个值同时设置现有的 I/O 和 full-memory decode 内部参数。

命令行以 `--threads` 作为主要参数名。现有的 `--prefetch-threads` 和 `--decode-threads` 继续作为同一个参数的兼容别名。若用户同时传入相互冲突的别名，由 CLI11 直接报错，不再静默选择其中一个。

## 重复序列的页面展开

请求仍然先按 `seq_id` 排序。相同 `seq_id` 的请求作为一个连续 run 处理。

假设一条重复序列跨越页面 A 和 B，修改后按 page 优先生成 chunk：

```text
A(请求1), A(请求2), B(请求1), B(请求2)
```

不再按每条请求分别生成：

```text
A(请求1), B(请求1), A(请求2), B(请求2)
```

输出仍通过 `original_pos` 写回用户原来的请求顺序。这样可以保持 chunk 的 page ID 全局单调，并避免重复的跨页序列把同一个物理页面拆成多个 `PageGroup`。

`batch_requested_residues` 和 `batch_multi_page_sequences` 等统计仍按用户的逻辑请求数量计算，不按去重后的序列 ID 数量计算。

## 相邻 miss 页面合并

完成 resident cache 的 hit/miss 分类后，只有满足以下条件的缺失页面才合并为一个 `PrefetchRange`：

- page ID 严格连续；
- 中间没有 cache hit 页面或被跳过的页面；
- 一个 range 最多包含 16 个完整的 4 KiB 页面，总计最多 64 KiB；
- 文件末尾不足 4 KiB 的短页面不参与合并，单独走 buffered pread fallback。

不会读取没有被请求的页面，因此合并后 `prefetch_read_bytes` 不会增加。

一个合并后的 `PrefetchRange` 保存：

- 第一个 group；
- group 数量；
- 第一个 page ID；
- 实际读取字节数；
- 一个 16-bit 的逐页 LRU admission mask。

I/O 完成后，解码器依次处理 range 中的所有 group，并通过页面在 range buffer 中的偏移找到对应 packed page。

cache admission、hit 和 miss 仍按物理页面计数。`prefetch_ranges` 改为表示真正提交给内核的 I/O range 数量。

预取 slot 的固定跨度为 64 KiB。slot 数量仍受现有 `prefetch_memory_bytes` 总预算限制，因此不会突破用户配置的临时内存上限。

完整且对齐的 range 使用当前选择的异步 I/O backend。文件末尾的短页面不能满足 O_DIRECT 的长度对齐要求，因此通过已经打开的 buffered `pages_fd_` 读取，之后进入相同的解码流程。

## 错误处理

继续保留现有的：

- 元数据边界检查；
- 空输出指针检查；
- short read 检查；
- worker 异常传播；
- LRU cache 状态校验。

合并后的 range 还要检查其中每个 group 的 page ID 是否严格连续，以及每个 page 是否位于已完成读取的 buffer 范围内。

## 验证方式

增加确定性的回归测试，覆盖以下情况：

- 同一个跨越两个或更多页面的序列 ID 重复出现时，每个物理页面只产生一次 miss；
- 四个连续缺失页面只产生一个 prefetch range；
- 十七个连续缺失页面产生两个 range；
- 空序列、单页序列、跨页序列和重复序列的结果都与逐条 `get()` 完全一致；
- 每条 `fetch()` 结果末尾都有 `\0`，且 `FetchResult::output_ptrs` 保持为空；
- full-memory 解码实际使用统一配置的线程数；
- prefix 和 LRU 两种 cache 模式返回完全相同的序列。

性能验证继续使用现有的 1M 和 10M 随机批量测试，对比修改前后的：

- 输出准备时间；
- page miss 数量；
- 实际 I/O range 数量；
- 读取字节数；
- 解码时间；
- wall time；
- 峰值 RSS。
