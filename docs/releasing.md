# Versioning and release validation

`VERSION` is the source of the CMake project version, Python package version,
and Python extension version. Update it once when preparing a release, and
record user-visible changes in `CHANGELOG.md`.

## Compatibility

AAStore currently targets Linux with a C++17 GCC or Clang toolchain and Python
3.9 or newer. CI defines the combinations actually checked. Scalar fallback
and runtime SIMD dispatch cover CPU instruction-set differences; they do not
establish support for another operating system.

During the 0.x series, minor versions may change the public API. Patch versions
are intended to preserve source compatibility within the same minor series.
The installed CMake package accepts compatible versions from that minor series.
The shared-library SONAME includes both the major and minor version, so a new
minor version does not silently replace the previous ABI.

AAStore does not promise binary interchangeability across different C++
standard libraries, compiler ABIs, or build options. Rebuild applications when
changing one of these. Changes to exposed option, statistics, or batch layouts
must be reviewed as ABI changes even if the code still compiles.

Database formats have their own identifiers: `AAPCMP3` for sequence metadata
and `AATEXT1` for optional text columns. Package version changes do not by
themselves require a database rebuild. A format change must document whether
existing data remains readable and how to migrate it.

## Before a release

1. Run the C++ CI matrix for static/shared libraries with io_uring enabled and
   disabled. An io_uring test may explicitly skip when the host kernel blocks
   the API; the pread path must still pass.
2. Install the SDK into a fresh prefix, move that prefix, and build and run
   `tests/install_consumer` and `examples/cpp` through `find_package`.
3. Build an sdist, unpack it outside the checkout, build a wheel from that
   source tree, install the wheel into a clean environment, and run the Python
   tests. Check that type declarations are present.
4. Use the portable-wheel workflow for redistributable Linux wheels. It
   disables io_uring, uses pread, and repairs the wheel through auditwheel.
   A local source build can enable io_uring; that wheel may require liburing
   on the destination machine and must not be presented as a portable wheel.
5. Review the changelog, version, license metadata, and artifact contents.
   The source distribution must exclude local build trees and paper assets.
   Create release tags and publish artifacts only as an explicitly authorized
   release action.

The workflows validate and upload build artifacts. They do not publish to
PyPI or create releases automatically.
