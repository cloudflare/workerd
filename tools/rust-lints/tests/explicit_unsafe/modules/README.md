# Supporting source modules

These files are module and include inputs for the parent explicit-unsafe
fixtures, not compilation roots. `inner.rs` declares a local file policy;
`outer.rs` relies on an outer policy in its parent when used by a passing
fixture. `inactive_file.rs` disables its own contents. Included fragments prove
that include! adds no root boundary while preserving authored module boundaries.

[`directory/`](directory/README.md) exercises `mod.rs` and nested file loading.
