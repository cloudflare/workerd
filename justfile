alias b := build
alias t := test
alias f := format
alias st := stream-test

default:
  @just --list

clang_version := "18"

prepare:
  cargo install gen-compile-commands

compile-commands:
  rm -f compile_commands.json | gen-compile-commands --root . --compile-flags compile_flags.txt --out compile_commands.json --src-dir ./src

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

format:
  python3 tools/cross/format.py

internal-pr:
  ./tools/unix/create-internal-pr.sh
