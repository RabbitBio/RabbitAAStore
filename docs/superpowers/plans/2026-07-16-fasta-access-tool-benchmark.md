# FASTA Access Tool Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run a reproducible AAStore, pyfastx, pyfaidx, samtools faidx, and SeqKit benchmark following the published index, random-access, iteration, time, and memory methodology while adding strict output validation.

**Architecture:** A small Python package owns request manifests, canonical FASTA hashing, backend commands, process measurement, cache hints, result persistence, and summaries. Each backend runs in a fresh measured process; large inputs, indexes, and raw results remain under `/home/bigssd`, while repository tests use a tiny deterministic FASTA fixture.

**Tech Stack:** Python 3 standard library, GNU `/usr/bin/time`, AAStore Python C API, pyfastx 2.3.1, pyfaidx 0.9.0.4, samtools 1.9, SeqKit 2.13.0, CMake.

## Global Constraints

- Use fixed random seed `123` and sampling without replacement.
- Run three measured repetitions and report mean, median, minimum, maximum, and standard deviation.
- Main results use one CPU and one tool thread; multithreaded SeqKit results remain separate.
- Report warm and per-file `POSIX_FADV_DONTNEED` cold-hint results separately.
- Name-based and numeric-ID AAStore results remain separate; numeric ID is not ranked against name lookup.
- Every ranked output must match record count, residue count, and SHA-256.
- 1M is correctness-only, 10M is the full matrix, and 100M is index plus 100K/1M random-access stress.
- Do not modify the AAStore on-disk format.
- Do not overwrite completed raw result JSON files when resuming.
- Delete measured FASTA output and process scratch files after their size and digest are atomically persisted; retain indexes, manifests, raw JSON, required failure logs, and reports.

---

### Task 1: Shared Data Model and Canonical FASTA Validation

**Files:**
- Create: `benchmarks/fasta_tools/__init__.py`
- Create: `benchmarks/fasta_tools/common.py`
- Create: `tests/fasta_tools_benchmark_regression.py`

**Interfaces:**
- Produces: `FastaRecord`, `DigestResult`, `iter_fasta()`, `canonical_digest()`, `read_names()`, `write_canonical_fasta()`.
- Consumes: ASCII FASTA paths and binary/text output streams.

- [ ] **Step 1: Write failing parser and digest tests**

Add a `unittest.TestCase` that writes wrapped records with descriptions and asserts:

```python
records = list(iter_fasta(fasta))
self.assertEqual([(r.name, r.sequence) for r in records], [
    ("alpha", "ACDEFG"),
    ("beta", "MNPQRST"),
])
digest = canonical_digest(records)
self.assertEqual(digest.records, 2)
self.assertEqual(digest.residues, 13)
```

Also assert that canonical output is exactly `>alpha\nACDEFG\n>beta\nMNPQRST\n` and malformed sequence-before-header input raises `ValueError`.

- [ ] **Step 2: Run the test and verify failure**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.CommonTests -v
```

Expected: import failure for `benchmarks.fasta_tools.common`.

- [ ] **Step 3: Implement the shared model**

Implement immutable records and streaming canonical hashing:

```python
@dataclass(frozen=True)
class FastaRecord:
    name: str
    sequence: str

@dataclass(frozen=True)
class DigestResult:
    records: int
    residues: int
    sha256: str

def write_canonical_fasta(records, output, hasher=None) -> DigestResult:
    digest = hasher or hashlib.sha256()
    count = residues = 0
    for record in records:
        payload = f">{record.name}\n{record.sequence}\n".encode("ascii")
        output.write(payload)
        digest.update(payload)
        count += 1
        residues += len(record.sequence)
    return DigestResult(count, residues, digest.hexdigest())
```

`iter_fasta()` must stream records, use the first whitespace-delimited header token, remove ASCII line endings, reject sequence text before a header, and never retain the full file.

- [ ] **Step 4: Run the common tests**

Run the command from Step 2. Expected: all `CommonTests` pass.

- [ ] **Step 5: Commit the shared model**

```bash
git add benchmarks/fasta_tools/__init__.py benchmarks/fasta_tools/common.py tests/fasta_tools_benchmark_regression.py
git commit -m "test: add FASTA benchmark canonical validation"
```

### Task 2: Deterministic Request Manifest Generation

**Files:**
- Create: `benchmarks/fasta_tools/generate_requests.py`
- Modify: `tests/fasta_tools_benchmark_regression.py`

**Interfaces:**
- Consumes: `generate_requests(fasta: Path, record_count: int, counts: list[int], seed: int, output_dir: Path)`.
- Produces: `requests_<count>_random.tsv`, `requests_<count>_source.tsv`, `.names.txt`, and `.ids.u64` files with zero-based IDs.

- [ ] **Step 1: Add failing deterministic manifest tests**

Create ten fixture records, call `generate_requests(..., counts=[3, 5], seed=123)`, and assert each random manifest has unique IDs, every name matches the FASTA record at that ID, rerunning produces identical bytes, and the source manifest IDs are sorted.

- [ ] **Step 2: Verify the manifest tests fail**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.ManifestTests -v
```

Expected: import failure for `generate_requests`.

- [ ] **Step 3: Implement one-pass manifest generation**

Use `random.Random(seed).sample(range(record_count), count)` for each count, union selected IDs, scan FASTA once, and retain only selected `(id, name, length)` tuples. Write TSV with header `id\tname\tlength`, text names one per line, and little-endian `struct.pack("<Q", id)` numeric IDs. Fail if scanned record count differs from `record_count`.

- [ ] **Step 4: Run deterministic tests**

Run the command from Step 2. Expected: all `ManifestTests` pass.

- [ ] **Step 5: Commit manifest generation**

```bash
git add benchmarks/fasta_tools/generate_requests.py tests/fasta_tools_benchmark_regression.py
git commit -m "feat: generate deterministic FASTA benchmark requests"
```

### Task 3: Backend Workers and Equivalent Output

**Files:**
- Create: `benchmarks/fasta_tools/worker.py`
- Create: `benchmarks/fasta_tools/backends.py`
- Modify: `tests/fasta_tools_benchmark_regression.py`

**Interfaces:**
- Produces: `BackendConfig`, `backend_command(config, task, manifest, output, digest_path) -> list[str]`.
- Produces CLI: `python -m benchmarks.fasta_tools.worker --backend BACKEND --task fetch|iterate ...`.
- Worker writes canonical FASTA when `--output` is a file and always writes `DigestResult` JSON to `--digest`.

- [ ] **Step 1: Add failing backend equivalence tests**

Build a four-record AAStore fixture with `paged_aatools build-fa --name-index`, build `.fxi` and `.fai` indexes, request `gamma, alpha, delta`, and run available workers. Parse each digest JSON and assert all backends report three records, the same residue count, and the same SHA-256. Skip a backend only when its executable/module path was not supplied explicitly.

- [ ] **Step 2: Verify backend tests fail**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.BackendTests -v
```

Expected: import failure for `benchmarks.fasta_tools.backends`.

- [ ] **Step 3: Implement Python backend workers**

Implement:

```python
def pyfastx_records(fasta, names):
    db = pyfastx.Fasta(str(fasta))
    for name in names:
        yield FastaRecord(name, db[name].seq)

def pyfaidx_records(fasta, names):
    db = pyfaidx.Fasta(str(fasta), as_raw=True, sequence_always_upper=False)
    try:
        for name in names:
            yield FastaRecord(name, str(db[name]))
    finally:
        db.close()

def aastore_records(store, ids, memory_mb, prefetch_mb):
    db = aastore.open(str(store), memory_mb=memory_mb,
                      prefetch_mb=prefetch_mb)
    sequences = db.fetch_str(ids)
    for name, sequence in zip(read_names_for_ids(store, ids), sequences):
        yield FastaRecord(name, sequence)
```

For AAStore name mode, read manifest names, resolve them through the store name API inside the measured process, then batch fetch. For numeric mode, consume `.ids.u64` directly and label the result `aastore_numeric`.

- [ ] **Step 4: Implement CLI backend adapters**

Use `samtools faidx -n 0 -r NAMES FASTA` and `seqkit faidx --threads 1 --infile-list NAMES FASTA`. Feed their FASTA stdout through `iter_fasta()` into the same canonical writer inside the measured worker. Propagate nonzero child status and stderr. `iterate` uses native streaming for Python backends and `seqkit seq --threads 1 --line-width 0`; samtools is marked unsupported for whole-file iteration rather than simulated with a generated all-name list.

- [ ] **Step 5: Run backend equivalence tests**

Run the command from Step 2 with `AASTORE_C_LIB`, `PYTHONPATH`, `PAGED_AATOOLS`, `SAMTOOLS`, and `SEQKIT` set. Expected: available backends pass and produce an identical digest.

- [ ] **Step 6: Commit backend workers**

```bash
git add benchmarks/fasta_tools/worker.py benchmarks/fasta_tools/backends.py tests/fasta_tools_benchmark_regression.py
git commit -m "feat: add equivalent FASTA benchmark backends"
```

### Task 4: Measurement, Cache Hints, Rotation, and Resume

**Files:**
- Create: `benchmarks/fasta_tools/run_benchmark.py`
- Modify: `tests/fasta_tools_benchmark_regression.py`

**Interfaces:**
- Produces: `drop_file_cache(path: Path) -> bool`, `rotate_backends(names: list[str], repetition: int) -> list[str]`, `run_measured(spec: RunSpec) -> RunResult`.
- CLI writes one immutable JSON file per backend/task/count/cache/repetition under `raw/`.

- [ ] **Step 1: Add failing orchestration tests**

Assert rotations for three backends are `[a,b,c]`, `[b,c,a]`, `[c,a,b]`; existing result files are returned as `status=skipped_existing`; failed commands preserve exit code and stderr path; `drop_file_cache()` returns `False` for a missing file and does not raise.

- [ ] **Step 2: Verify orchestration tests fail**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.RunnerTests -v
```

Expected: import failure for `run_benchmark`.

- [ ] **Step 3: Implement measured execution**

Wrap every worker with:

```bash
/usr/bin/time -v -o TIME_FILE taskset -c CPU WORKER_COMMAND
```

Parse elapsed time, user/system seconds, maximum RSS, major/minor faults, filesystem inputs/outputs, and exit status. Record `time.perf_counter()` wall time independently. Use `subprocess.run(check=False)` and atomically rename `result.json.part` to `result.json` only after collecting stderr, digest, command, environment, input/index sizes, and cache-hint outcomes.

Create measured FASTA, GNU time, digest, and stderr scratch paths inside a per-run
`TemporaryDirectory` under the result filesystem. Wrap execution in `try/finally` so
success, failure, timeout, and interruption remove the scratch directory after the
required digest and failure excerpt have been persisted. Never glob or recursively
delete outside that per-run directory.

- [ ] **Step 4: Implement experiment expansion**

Accept explicit `--dataset-label`, `--fasta`, `--store`, `--request-dir`, `--backends`, `--tasks`, `--counts`, `--cache-states`, `--repetitions`, `--cpu`, and `--output-dir`. Warm mode performs one unmeasured worker run per cell. Cold mode applies `posix_fadvise(..., POSIX_FADV_DONTNEED)` to each backend's FASTA/store/index files before process launch. Never call privileged global cache-dropping interfaces.

- [ ] **Step 5: Run runner tests and a tiny measured smoke**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.RunnerTests -v
python3 -m benchmarks.fasta_tools.run_benchmark \
  --dataset-label tiny --fasta /tmp/fasta-tools-test/proteins.fa \
  --store /tmp/fasta-tools-test/store \
  --request-dir /tmp/fasta-tools-test/requests \
  --backends pyfastx,pyfaidx,samtools,seqkit,aastore_name_0 \
  --tasks fetch --counts 3 --cache-states warm --repetitions 1 \
  --output-dir /tmp/fasta-tools-test/results
```

Expected: one successful immutable result JSON per available backend, identical digest values,
and no measured FASTA, GNU time, digest, stderr, or `.part` scratch files left behind.

- [ ] **Step 6: Commit orchestration**

```bash
git add benchmarks/fasta_tools/run_benchmark.py tests/fasta_tools_benchmark_regression.py
git commit -m "feat: orchestrate reproducible FASTA tool measurements"
```

### Task 5: Environment Capture, Summaries, and Documentation

**Files:**
- Create: `benchmarks/fasta_tools/install_tools.sh`
- Create: `benchmarks/fasta_tools/capture_environment.py`
- Create: `benchmarks/fasta_tools/summarize.py`
- Create: `benchmarks/fasta_tools/README.md`
- Modify: `tests/fasta_tools_benchmark_regression.py`

**Interfaces:**
- Installer creates `.venv_fasta_tools` and `.tools/seqkit-2.13.0/seqkit`.
- Summary consumes `raw/*.json` and emits `summary.csv`, `summary.json`, and `validation.json`.

- [ ] **Step 1: Add failing summary tests**

Write three synthetic successful run JSON files and one checksum-mismatch file. Assert summary statistics equal known mean/median/min/max/sample standard deviation and the mismatch cell appears in `validation.json` but not the ranked summary.

- [ ] **Step 2: Verify summary tests fail**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression.SummaryTests -v
```

Expected: import failure for `summarize`.

- [ ] **Step 3: Implement pinned installation**

The installer must execute:

```bash
python3 -m venv .venv_fasta_tools
.venv_fasta_tools/bin/pip install --upgrade pip
.venv_fasta_tools/bin/pip install pyfastx==2.3.1 pyfaidx==0.9.0.4
curl -fL -o seqkit_linux_amd64.tar.gz \
  https://github.com/shenwei356/seqkit/releases/download/v2.13.0/seqkit_linux_amd64.tar.gz
echo '7d686de448464fada1b1988e2e07d693bec68768312da62846bc0e2b502bfc46  seqkit_linux_amd64.tar.gz' | sha256sum -c -
```

Extract into `.tools/seqkit-2.13.0`, print all versions, and leave downloaded archives outside git tracking.

- [ ] **Step 4: Implement environment capture and summaries**

Capture UTC timestamp, hostname, `git rev-parse HEAD`, `git status --porcelain`, Python/package/tool versions, `uname -a`, `lscpu`, `/proc/meminfo`, `df -T`, and executable SHA-256 values in JSON. Group successful runs by dataset/task/count/cache/backend, verify all ranked digests match the reference backend, and calculate statistics with `statistics.mean`, `median`, `stdev`, `min`, and `max`.

- [ ] **Step 5: Document exact commands and interpretation rules**

README sections must cover setup, tiny regression, index build, request generation, 10M matrix, 100M stress, resume behavior, result schema, output correctness, cold-cache limitation, name-vs-ID separation, unsupported cells, and paper-method correspondence.

- [ ] **Step 6: Run all benchmark regression tests**

Run:

```bash
python3 -m unittest tests.fasta_tools_benchmark_regression -v
```

Expected: all tests pass; unavailable external tools are explicit skips.

- [ ] **Step 7: Commit reporting and documentation**

```bash
git add benchmarks/fasta_tools/install_tools.sh benchmarks/fasta_tools/capture_environment.py benchmarks/fasta_tools/summarize.py benchmarks/fasta_tools/README.md tests/fasta_tools_benchmark_regression.py
git commit -m "docs: add reproducible FASTA tool benchmark workflow"
```

### Task 6: Build Indexes, Validate 1M, and Run the Formal Matrix

**Files:**
- Create outside git: `/home/bigssd/gaoyijie_fasta_tool_benchmark_20260716/`
- Produce: `environment.json`, `requests/`, `indexes/`, `raw/`, `summary.csv`, `summary.json`, `validation.json`, and logs.

**Interfaces:**
- Consumes the scripts from Tasks 1-5 and existing 1M/10M/100M FASTA inputs.
- Produces immutable raw measurements and validated summaries.

- [ ] **Step 1: Build AAStore and install pinned competitors**

Run:

```bash
cmake -S . -B build-fasta-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-fasta-bench -j16
bash benchmarks/fasta_tools/install_tools.sh
python3 -m benchmarks.fasta_tools.capture_environment \
  --output /home/bigssd/gaoyijie_fasta_tool_benchmark_20260716/environment.json
```

Expected: `paged_aatools`, `libaastore_c.so`, pyfastx 2.3.1, pyfaidx 0.9.0.4, samtools 1.9, and SeqKit 2.13.0 are recorded.

- [ ] **Step 2: Run the 1M correctness gate**

Build fresh indexes/stores under `indexes/1m`, generate 1K requests, then run all backends once with canonical output enabled. Expected: every supported backend has identical record count, residue count, and SHA-256. Stop before performance tests on any mismatch.

- [ ] **Step 3: Build fresh 10M indexes and record construction metrics**

Run each builder through the same measured-run machinery with one thread where supported. Keep AAStore numeric store size and name-index size separate. Run three fresh builds per backend in independent directories or remove only that backend's generated index between repetitions. Expected: three valid build JSON files per backend or an explicit timeout/OOM result.

- [ ] **Step 4: Generate 10M request sets**

Run:

```bash
.venv_fasta_tools/bin/python -m benchmarks.fasta_tools.generate_requests \
  --fasta /home/bigssd/test_cdhit/10m_nr.fa --record-count 10000000 \
  --counts 1000,10000,100000,1000000,3000000 --seed 123 \
  --output-dir /home/bigssd/gaoyijie_fasta_tool_benchmark_20260716/requests/10m
```

Expected: random and source-order manifests with matching name/ID pairs for all five counts.

- [ ] **Step 5: Run the 10M complete matrix**

Run fetch for all counts, iterate for supported backends, and canonical FASTA extraction for source-order 1K/10K/100K/1M lists. Use warm and cold-hint states, three repetitions, rotating backend order, CPU affinity, one thread, and immutable raw JSON outputs. Large canonical output files may be deleted only after their digest and byte count are committed to the run JSON.

After every matrix cell, assert its per-run scratch directory no longer exists. Preserve
only indexes/stores, manifests, immutable raw result JSON, necessary failure excerpts,
environment metadata, and reports.

- [ ] **Step 6: Summarize and inspect the 10M results**

Run:

```bash
.venv_fasta_tools/bin/python -m benchmarks.fasta_tools.summarize \
  --raw-dir /home/bigssd/gaoyijie_fasta_tool_benchmark_20260716/raw/10m \
  --output-dir /home/bigssd/gaoyijie_fasta_tool_benchmark_20260716/reports/10m
```

Expected: no ranked checksum mismatches; averages reproduce the paper reporting convention, while medians and dispersion remain available.

- [ ] **Step 7: Run the 100M stress subset**

Build fresh indexes with measured memory and time, generate 100K/1M requests, then run only fetch in warm and cold-hint states for three repetitions. Preserve timeout and OOM outcomes. Do not run full iteration or 30% extraction at 100M.

- [ ] **Step 8: Perform final verification**

Run all Python regressions, inspect `validation.json`, count expected raw cells, verify no `.part` files remain, and run `git diff --check`. Record incomplete 100M jobs as incomplete rather than extrapolating results.
