alias b := build
alias t := test

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
