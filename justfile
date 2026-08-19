alias b := build
alias t := test
alias f := format
alias st := stream-test
alias c := coverage
alias w := watch

default:
  @just --list

pwd := `pwd`

prepare:
  @if [ "{{os()}}" = "macos" ]; then just prepare-macos; elif [ "{{os()}}" = "linux" ]; then just prepare-ubuntu; else echo "Unsupported OS: {{os()}}"; exit 1; fi
  cargo install gen-compile-commands watchexec-cli
  just create-external
  just compile-commands
  just prepare-rust

prepare-rust:
  rustup install 1.91.0
  rustup component add rust-analyzer --toolchain 1.91.0

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
  bazel test {{args}} --test_output=streamed --nocache_test_results --test_tag_filters= --test_size_filters=

# e.g. just node-test zlib
node-test test_name *args:
  just stream-test //src/workerd/api/node/tests:{{test_name}}-nodejs-test@ {{args}}

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
  bazel test //src/wpt:{{test_name}}@ --test_env=GEN_TEST_CONFIG=1 --test_output=streamed

# Specify the full Bazel target name for the test to be created.
# e.g. just new-test //src/workerd/api/tests:v8-temporal-test
new-test test_name:
  ./tools/unix/new-test.sh {{test_name}}

format:
  python3 tools/cross/format.py

internal-pr:
  ./tools/unix/create-internal-pr.sh

# update dependencies with a given prefix (all by default)
update-deps prefix="":
  ./build/deps/update-deps.py {{prefix}}

# equivalent to `cargo update`; use `workspace` or <package> to limit update scope
update-rust package="":
  bazel run @rules_rust//tools/upstream_wrapper:cargo -- update --manifest-path=deps/rust/Cargo.toml {{package}}

# example: just bench mimetype
bench path:
  bazel run //src/workerd/tests:bench-{{path}} --config=benchmark

# example: just clippy dns
clippy package="...":
  bazel build //src/rust/{{package}} --config=lint

# example: just clang-tidy //src/rust/jsg:ffi
clang-tidy target="//...":
  bazel build {{target}} --config=clang-tidy

generate-types:
  bazel build //types
  chmod -R u+w types/generated-snapshot 2>/dev/null || true
  rm -rf types/generated-snapshot
  mkdir -p types/generated-snapshot
  cp -r bazel-bin/types/definitions/. types/generated-snapshot/

update-reported-node-version:
  python3 tools/update_node_version.py src/workerd/api/node/node-version.h

update-opencode:
  python3 tools/update_opencode_version.py

# called by rust-analyzer discoverConfig (quiet recipe with no output)
# rust-analyzer doesn't like stderr output, redirect it to /dev/null
@_rust-analyzer:
  rm -rf ./rust-project.json
  bazel run @rules_rust//tools/rust_analyzer:discover_bazel_rust_project 2>/dev/null

create-external:
  tools/unix/create-external.sh

bench-all:
  bazel query 'attr(tags, "[\[ ]google_benchmark[,\]]", //... + @capnp-cpp//...)' --output=label | xargs -I {} bazel run --config=benchmark {}

lint: eslint

eslint:
  just stream-test \
    //src/cloudflare:cloudflare@eslint \
    //src/node:node@eslint \
    //src/pyodide:pyodide_static@eslint \
    //src/wpt:wpt-all@tsproject@eslint \
    //types:types_lib@eslint

# Generate code coverage report (Linux only)
coverage path="//...":
  bazel coverage --config=coverage {{path}}
  genhtml --branch-coverage --ignore-errors category --output coverage "$(bazel info output_path)/_coverage/_coverage_report.dat"
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

# Run the @gc-stress test variants to detect GC-related bugs (premature collection,
# missing visitForGc, etc). Debug builds only. These variants are tagged off-by-default
# so they won't run in normal `just test` invocations.
# e.g. just test-gc-stress //src/workerd/api/tests:all
test-gc-stress *args="//src/...":
  bazel test $(bazel query 'attr(tags, gc-stress, kind("_wd_test rule", {{args}}))' 2>/dev/null) --test_tag_filters=gc-stress

# Like test-gc-stress but with AddressSanitizer to detect use-after-free.
# Leak detection is disabled to avoid masking more serious issues.
test-gc-stress-asan *args="//src/...":
  bazel test $(bazel query 'attr(tags, gc-stress, kind("_wd_test rule", {{args}}))' 2>/dev/null) --config=asan --test_tag_filters=gc-stress,-no-asan --test_timeout=300 --test_env=ASAN_OPTIONS=detect_leaks=0

# "Super" GC stress mode: forces a full GC before *every* js.alloc<>(), catching
# collectible-but-still-used-within-a-synchronous-task use-after-frees that the turn-boundary
# gc-stress mode cannot find. This is brutally slow (a full GC on one of the hottest paths in the
# API layer), so it defaults to a small target set rather than the whole suite. Only compiled in under ASAN
# (hence --config=asan is required); V8 is initialized with --expose-gc automatically when the env
# var is set, so tests do not need to set it themselves.
# e.g. just test-gc-stress-alloc-asan //src/workerd/api/tests:abortsignal-test@
test-gc-stress-alloc-asan *args="//src/workerd/api/tests:abortsignal-test@":
  bazel test --config=asan --test_timeout=900 --test_env=WORKERD_GC_STRESS_ALLOC=1 --test_env=ASAN_OPTIONS=detect_leaks=0 {{args}}

# Targets known to be INCOMPATIBLE with alloc-stress. These are NOT memory-safety failures (the
# full sweep found zero ASAN errors); they fail for one of three reasons:
#   * GC-mechanics tests: they assert precise collection behavior (e.g. "object is NOT collected
#     until an explicit gc()", or "a *minor* GC collects X"). Forcing a full GC before every
#     allocation collects/tenures objects early, which is exactly what these tests check against.
#       - //src/workerd/jsg:tracing-test           (asserts !siblingCollected, minor-GC specifics)
#       - //src/rust/jsg-test:jsg-test_test        (asserts no collection before explicit request_gc)
#   * Timing-sensitive behavior the aggressive GC perturbs (no UAF; abort/trace-span/connection
#     ordering shifts). js-rpc's portAbortCall is already marked TODO(bug) for abort-reason
#     propagation upstream.
#       - //src/workerd/api/tests:js-rpc-test       //src/workerd/api/tests:js-rpc-socket-test
#       - //src/workerd/api/tests:tail-worker-test  //src/wpt:fetch/api
#   * Simply too slow to finish a full GC at every allocation within any reasonable timeout.
#       - //src/wpt:streams
# Regex (matched against full test-target labels, including @-variants generated by the wd_test
# macro) selecting the incompatible targets to drop from the sweep.
gc-alloc-stress-exclude-re := ":tracing-test@|:jsg-test_test|:js-rpc-test@|:js-rpc-socket-test@|:tail-worker-test@|fetch/api@|:streams@"

# Broad alloc-stress sweep over a target set (default: all of //src/...), excluding the known
# incompatible targets above. Brutally slow; intended for occasional UAF hunts, not CI.
# e.g. just test-gc-stress-alloc-asan-sweep //src/workerd/api/...
test-gc-stress-alloc-asan-sweep *args="//src/...":
  bazel test --config=asan --keep_going --test_timeout=900 --test_env=WORKERD_GC_STRESS_ALLOC=1 --test_env=ASAN_OPTIONS=detect_leaks=0 \
    $(bazel query 'tests(set({{args}})) except filter("{{gc-alloc-stress-exclude-re}}", tests(set({{args}})))' 2>/dev/null)

watch *args="build":
  watchexec -rc -w src -w build just {{args}}
