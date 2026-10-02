# Changelog

## Unreleased

- Centralize option validation and reject unknown storage modes, I/O backends,
  and read strategies before file access or output writes, including empty reads.
- Expose optional `validate()` checks for C++ and Python configuration objects
  while preserving documented zero-valued read budgets and thread settings.
- Select Python development components compatible with CMake 3.16/3.17.
- Split Store tests into independently selectable CTest groups.
- Expose owner-preserving, read-only `offsets` and `lengths` memoryviews for
  Python sequence and text batches, using native uint64 byte coordinates.
- Match Python statistics type declarations to their read-only runtime fields.
- Describe pybind11 enums, positional record accessors, and nonconstructible
  result types accurately in Python type declarations.
- Add a practical Python API guide and self-contained examples for records,
  batch buffer layouts, and reusable output buffers; run the examples in CI.
- Cover Python batch layout lifetimes, UTF-8 byte boundaries, Scan reads,
  sorted requests, and range budgets with installed-package tests.
- Add an installable C++ SDK with the `aastore::aastore` CMake target,
  relocatable package configuration, package version checks, and pkg-config metadata.
- Separate library, command-line tool, benchmark, and test build options;
  disable auxiliary targets by default when embedded as a subdirectory.
- Add an installed-library C++ example and tests for external consumers.
- Add Linux static/shared and I/O backend CI coverage, Python source-package
  rebuild tests, and a manually triggered portable-wheel build.
- Use `VERSION` as the C++ and Python version source; document compatibility
  expectations and release validation.
- Limit Python wheels to the Python installation component.
- Anchor source-package inclusion rules to exclude local build trees, paper
  assets, and Python bytecode; check the archive contents in CI.
- Read unaligned metadata block bases safely without changing AAPCMP3 files.
- Join active read workers when thread startup or the calling worker fails.
- Add the GPL-3.0-only license and matching package metadata.
- Detect runtime io_uring availability in integration tests so restricted
  kernels still validate the pread and automatic fallback paths.

## 0.1.0 source baseline

- Continuous AAPCMP3 5-bit sequence storage with compact boundary metadata.
- Disk, fixed-prefix memory, and full-memory read modes; SIMD runtime dispatch.
- Random batch reads, sequential reads, caller-provided buffers, and optional
  name and description columns.
- Python bindings with buffer views, type declarations, and concurrent read support.

The repository also contains the earlier `v0.0.1` tag. This file records the
current source baseline and subsequent changes; it does not assert that a
0.1.0 binary release has already been published.
