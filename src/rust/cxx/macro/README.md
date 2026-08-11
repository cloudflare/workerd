# CXX bridge procedural macro

This directory contains the procedural macro implementation for workerd's in-tree CXX fork. Users
do not depend on it directly; the `//src/rust/cxx:cxx` crate reexports the macro from
`//src/rust/cxx:cxxbridge-macro`.

The macro shares the parser and generator used by the bridge command-line tool. It is built with
workerd's Rust toolchain and vendored crate dependencies rather than through the former external
`workerd-cxx` Bazel repository.
