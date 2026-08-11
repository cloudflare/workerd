# CXX C++ code generator

This directory contains the in-tree CXX fork's C++ code generator. Its command-line frontend is in
`cmd/`, the build-script frontend is in `build/`, and the embeddable frontend is in `lib/`.

Workerd uses the command-line frontend through `//src/rust/cxx:codegen` when Bazel's
`wd_rust_crate` and `wd_rust_binary` rules generate bridge headers and sources. These sources moved
into workerd from the former external `workerd-cxx` repository and use workerd's Rust dependency
set and toolchain.
