default:
  @just --list

# Override this value by calling `just --set clang_version 18`
clang_version := "15"

prepare:
  cargo install gen-compile-commands

compile-commands:
  rm -f compile_commands.json | gen-compile-commands --root . --compile-flags compile_flags.txt --out compile_commands.json --src-dir ./src

clean:
  rm -f compile_commands.json

build *args="//...":
  bazel build {{args}} --action_env=CC=clang-{{clang_version}} --action_env=CXX=clang++-{{clang_version}}

build-asan *args="//...":
  just build {{args}} --config=asan --sandbox_debug

test *args="//...":
  bazel test {{args}} --action_env=CC=clang-{{clang_version}} --action_env=CXX=clang++-{{clang_version}} --test_env=LLVM_SYMBOLIZER=llvm-symbolizer-{{clang_version}}

test-asan *args="//...":
  just test {{args}} --config=asan
