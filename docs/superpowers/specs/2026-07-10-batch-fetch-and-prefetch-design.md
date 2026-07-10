# AAStore Batch Fetch and Prefetch Design

## Goal

Reduce full-memory output preparation cost and no/half-memory page I/O overhead without changing decoded sequence contents, request ordering, cache capacity, or caller-owned output semantics.

## Scope

The change covers four connected parts of `PagedProteinAAStore`:

1. `FetchResult` output preparation.
2. One unified batch worker count for resident decode and miss I/O.
3. Page-major expansion of repeated sequence IDs.
4. Bounded coalescing of adjacent missing pages.

Hot sequence overlay policy, physical page layout, encoding, metadata format, and build pipeline are outside this change.

## FetchResult Output

`FetchResult::storage` uses `AANoInitAllocator<char>` so resizing an arena that will be fully overwritten does not first clear the payload. `offsets` continues to describe results in caller request order.

`FetchResult::output_ptrs` remains present for source compatibility but is not populated by `fetch()` or `fetch_into()`. Internal decode uses `ArenaOutputSource { storage.data(), offsets.data() }` directly.

Every non-empty sequence chunk that finishes an output writes its trailing `\0`. Empty sequences receive their terminator during arena preparation. The post-decode `terminate_fetch_result()` pass is removed from this path.

The returned views and strings remain byte-for-byte identical. Reusing a `FetchResult` retains vector capacity across calls.

## Unified Threads

`Options::io_threads` is the unified batch worker count. It controls asynchronous miss I/O streams, resident cache hit decode, and full raw cache decode. `configure()` sets both existing internal thread settings from this value.

The command-line tool exposes `--threads` as the primary spelling. Existing `--prefetch-threads` and `--decode-threads` spellings remain accepted as aliases to the same value. Supplying conflicting aliases is rejected by CLI11 rather than silently selecting one.

## Repeated Sequence Expansion

Requests are sorted by sequence ID as before. Equal sequence IDs are processed as one run. For a run spanning pages A and B, chunks are emitted page-major:

```text
A(request 1), A(request 2), B(request 1), B(request 2)
```

instead of request-major:

```text
A(request 1), B(request 1), A(request 2), B(request 2)
```

This preserves output ordering through `original_pos`, keeps chunks globally page-monotonic, and prevents the same page from becoming multiple page groups solely because a multi-page sequence ID was repeated.

Requested residue and multi-page sequence statistics continue to count logical requests, not unique sequence IDs.

## Adjacent Miss Coalescing

After resident cache hit/miss classification, consecutive missing page groups are merged into one prefetch range when:

- page IDs are exactly adjacent;
- there is no cached or skipped page between them;
- the merged range contains at most 16 full 4 KiB pages (64 KiB total);
- the final short page is kept as a single-page buffered fallback range.

No unrequested page is read. Therefore coalescing cannot increase `prefetch_read_bytes`.

A prefetch range stores its first group, group count, first page, byte count, and a 16-bit per-page LRU admission mask. Decode walks all groups in the range and selects each packed page at its offset inside the range buffer. Cache admission and hit/miss statistics remain page based; `prefetch_ranges` becomes the actual number of I/O ranges.

The fixed prefetch slot stride is 64 KiB. The existing total prefetch memory budget still caps slot count, so coalescing does not increase the configured temporary memory limit. Full aligned ranges use the selected asynchronous backend. A final short page cannot satisfy O_DIRECT length alignment, so it is read through the already-open buffered `pages_fd_` and then enters the same decode path.

## Error Handling

Existing metadata bounds, output null checks, short-read detection, worker exception propagation, and LRU cache validation remain active. A merged range additionally validates that every contained group has the expected consecutive page ID and lies within the completed buffer.

## Verification

Add deterministic regression coverage for:

- repeated IDs whose sequence crosses two or more pages produce one miss per physical page;
- four adjacent missing pages produce one prefetch range;
- seventeen adjacent missing pages produce two ranges;
- empty, single-page, multi-page, and repeated sequences match scalar `get()` output;
- every fetched sequence has a trailing `\0` and `FetchResult::output_ptrs` remains empty;
- full-memory decode reports the configured unified worker count;
- prefix and LRU cache modes return identical sequences.

Performance verification uses the existing 1M/10M random batch benchmarks and compares output preparation time, page misses, actual ranges, bytes read, decode time, wall time, and peak RSS against the pre-change measurements.
