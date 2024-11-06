alias b := build
alias t := test
alias f := format
alias st := stream-test

default:
  @just --list

pwd := `pwd`
clang_version := "18"

prepare:
  cargo install gen-compile-commands

compile-commands:
  rm -f compile_commands.json | gen-compile-commands --root {{pwd}} --compile-flags compile_flags.txt --out compile_commands.json --src-dir {{pwd}}/src

clean:
  rm -f compile_commands.json

build *args="//...":
  bazel build {{args}}

build-asan *args="//...":
  just build {{args}} --config=asan --sandbox_debug

test *args="//...":
  bazel test {{args}} --test_env=LLVM_SYMBOLIZER=llvm-symbolizer-{{clang_version}}

test-asan *args="//...":
  just test {{args}} --config=asan

# e.g. just stream-test //src/cloudflare:cloudflare.capnp@eslint
stream-test args:
  bazel test {{args}} --test_output=streamed --test_env=LLVM_SYMBOLIZER=llvm-symbolizer-{{clang_version}}

# e.g. just node-test zlib
node-test test_name:
  just stream-test //src/workerd/api/node:tests/{{test_name}}-nodejs-test

format: rustfmt
  python3 tools/cross/format.py

internal-pr:
  ./tools/unix/create-internal-pr.sh

# update dependencies with a given prefix (all by default)
update-deps prefix="":
  ./build/deps/update-deps.py {{prefix}}

# equivalent to `cargo update`; use `workspace` or <package> to limit update scope
update-rust package="full":
  bazel run //deps/rust:crates_vendor -- --repin {{package}}

rust-analyzer:
  bazel run @rules_rust//tools/rust_analyzer:gen_rust_project

rustfmt:
  bazel run @rules_rust//:rustfmt

# example: just bench mimetype
bench path:
  bazel run //src/workerd/tests:bench-{{path}}
