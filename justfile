alias b := build
alias t := test
alias f := format
alias st := stream-test

default:
  @just --list

pwd := `pwd`

prepare:
  cargo install gen-compile-commands

compile-commands:
  rm -f compile_commands.json | gen-compile-commands --root {{pwd}} --compile-flags compile_flags.txt --out compile_commands.json --src-dir {{pwd}}/src

clean:
  rm -f compile_commands.json

build *args="//...":
  bazel build {{args}}

build-asan *args="//...":
  just build {{args}} --config=asan

test *args="//...":
  bazel test {{args}}

test-asan *args="//...":
  just test {{args}} --config=asan

# e.g. just stream-test //src/cloudflare:cloudflare.capnp@eslint
stream-test args:
  bazel test {{args}} --test_output=streamed

# e.g. just node-test zlib
node-test test_name:
  just stream-test //src/workerd/api/node:tests/{{test_name}}-nodejs-test

# e.g. just wpt-test urlpattern
wpt-test test_name:
  just stream-test //src/wpt:{{test_name}}

lldb-wpt-test test_name: build
  cd bazel-bin/src/wpt/{{test_name}}.runfiles/workerd/src/wpt; lldb ../workerd/server/workerd  -- test {{test_name}}.wd-test --experimental --directory-path=TEST_TMPDIR=/tmp

asan-wpt-test test_name:
  bazel test //src/workerd/api/wpt:{{test_name}} --config=asan

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

# example: just clippy dns
clippy package="...":
  bazel build //src/rust/{{package}} --config=lint
