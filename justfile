alias b := build
alias t := test
alias f := format
alias st := stream-test
alias c := coverage
alias w := watch
alias l := lint

default:
  @just --list

pwd := `pwd`

prepare:
  @if [ "{{os()}}" = "macos" ]; then just prepare-macos; elif [ "{{os()}}" = "linux" ]; then just prepare-ubuntu; else echo "Unsupported OS: {{os()}}"; exit 1; fi
  cargo install gen-compile-commands watchexec-cli
  just create-external
  just compile-commands

prepare-ubuntu:
  sudo apt-get install -y --no-install-recommends libc++abi1-19 libc++1-19 libc++-19-dev lld-19 bazelisk python3 lcov fd-find

prepare-macos:
  brew install --quiet bazelisk python3 lcov fd

compile-commands:
  rm -f compile_commands.json
  gen-compile-commands --root {{pwd}} --compile-flags compile_flags.txt --out compile_commands.json --src-dir {{pwd}}/src

clean:
  rm -f compile_commands.json

build *args="//...":
  bazel build {{args}}

# example: just watch run -- serve $(pwd)/samples/helloworld/config.capnp
run *args="-- --help":
  bazel run //src/workerd/server:workerd -- {{args}} --watch --verbose --experimental

build-asan *args="//...":
  just build {{args}} --config=asan

test *args="//...":
  bazel test {{args}}

test-asan *args="//...":
  just test {{args}} --config=asan

# e.g. just stream-test //src/cloudflare:cloudflare.capnp@eslint
stream-test *args:
  bazel test {{args}} --test_output=streamed --nocache_test_results --config=debug --test_tag_filters= --test_size_filters=

# e.g. just node-test zlib
node-test test_name *args:
  just stream-test //src/workerd/api/node/tests:{{test_name}}-nodejs-test {{args}}

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

generate-types:
  bazel build //types
  cp -r bazel-bin/types/definitions/latest types/generated-snapshot/
  cp -r bazel-bin/types/definitions/experimental types/generated-snapshot/

update-reported-node-version:
  python3 tools/update_node_version.py src/workerd/api/node/node-version.h

# called by rust-analyzer discoverConfig (quiet recipe with no output)
@_rust-analyzer:
  rm -rf ./rust-project.json
  # rust-analyzer doesn't like stderr output, redirect it to /dev/null
  bazel run @rules_rust//tools/rust_analyzer:discover_bazel_rust_project 2>/dev/null

create-external:
  tools/unix/create-external.sh

bench-all:
  bazel query 'deps(//src/workerd/tests:all_benchmarks, 1)' --output=label | grep -v 'all_benchmarks' | xargs -I {} bazel run --config=benchmark {}

eslint:
  just stream-test \
    //src/cloudflare:cloudflare@eslint \
    //src/node:node@eslint \
    //src/pyodide:pyodide_static@eslint \
    //src/wpt:wpt-all@eslint \
    //types:types_lib@eslint

coverage path="//...":
  bazel coverage {{path}}
  genhtml --branch-coverage --output coverage "$(bazel info output_path)/_coverage/_coverage_report.dat"
  open coverage/index.html

profile path:
  bazel run //src/workerd/tests:bench-{{path}} --config=benchmark --run_under="perf record -F max --call-graph lbr"
  PERF_FILE=$(fdfind perf.data bazel-bin | grep "{{path}}" | head -1); \
  if [ -n "$PERF_FILE" ] && [ -s "$PERF_FILE" ]; then \
    cp $PERF_FILE perf.data; \
    perf report --input=$PERF_FILE; \
  else \
    echo "No valid perf.data file found for {{path}}"; \
  fi

watch *args="build":
  watchexec -rc -w src -w build just {{args}}

lint: clippy eslint
