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
stream-test *args:
  bazel test {{args}} --test_output=streamed --cache_test_results=no --config=debug --test_tag_filters= --test_size_filters=

# e.g. just node-test zlib
node-test test_name *args:
  just stream-test //src/workerd/api/node:tests/{{test_name}}-nodejs-test {{args}}

# e.g. just wpt-test urlpattern
wpt-test test_name:
  just stream-test //src/wpt:{{test_name}}

lldb-wpt-test test_name: build
  cd bazel-bin/src/wpt/{{test_name}}.runfiles/workerd/src/wpt; lldb ../workerd/server/workerd  -- test {{test_name}}.wd-test --experimental --directory-path=TEST_TMPDIR=/tmp

asan-wpt-test test_name:
  bazel test //src/workerd/api/wpt:{{test_name}} --config=asan

new-wpt-test test_name:
  mkdir -p src/wpt/$(dirname {{test_name}})
  echo "export default {};" > src/wpt/{{test_name}}-test.ts
  git add src/wpt/{{test_name}}-test.ts

  echo >> src/wpt/BUILD.bazel
  echo 'wpt_test(name = "{{test_name}}", config = "{{test_name}}-test.ts", wpt_directory = "@wpt//:{{test_name}}@module")' >> src/wpt/BUILD.bazel

  ./tools/cross/format.py
  bazel test //src/wpt:{{test_name}} --test_env=GEN_TEST_CONFIG=1 --test_output=streamed

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

rustfmt:
  bazel run @rules_rust//:rustfmt

# example: just bench mimetype
bench path:
  bazel run //src/workerd/tests:bench-{{path}} --config=benchmark

# example: just clippy dns
clippy package="...":
  bazel build //src/rust/{{package}} --config=lint

prepare-ubuntu:
  sudo apt-get install -y --no-install-recommends libc++abi1-19 libc++1-19 libc++-19-dev lld-19 bazelisk python3

generate-types:
  bazel build //types:types
  cp -r bazel-bin/types/definitions/latest types/generated-snapshot/
  cp -r bazel-bin/types/definitions/experimental types/generated-snapshot/

# called by rust-analyzer discoverConfig (quiet recipe with no output)
@_rust-analyzer:
  rm -rf ./rust-project.json
  # rust-analyzer doesn't like stderr output, redirect it to /dev/null
  bazel run @rules_rust//tools/rust_analyzer:discover_bazel_rust_project 2>/dev/null

create-external:
  tools/unix/create-external.sh

bench-all:
  bazel query 'deps(//src/workerd/tests:all_benchmarks, 1)' --output=label | grep -v 'all_benchmarks' | xargs -I {} bazel run --config=benchmark {}
