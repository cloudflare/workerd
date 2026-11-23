#!/bin/bash
# Trains the profile that optimizes the Linux release builds (see build/pgo/README.md). Builds the
# release configuration with IR instrumentation, runs the tests that run the workerd binary, merges
# the raw profiles they write, and writes the result to WORK_DIR/workerd.profdata.zst, for
# build/pgo/upload.sh.
#
# Usage: build/pgo/train.sh WORK_DIR [BAZEL_FLAGS...]
#   e.g. build/pgo/train.sh /tmp/pgo-profile --config=bazelflare
#
# The profiles are merged with //build/pgo:llvm-profdata, which only runs on x64 Linux.
# Elsewhere, set LLVM_PROFDATA to an llvm-profdata from the same LLVM major version as the
# toolchain (build/deps/llvm.MODULE.bazel).

set -euo pipefail

if [[ $# -lt 1 ]]; then
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

OUT=$(mkdir -p "$1" && cd "$1" && pwd)
shift
cd "$(dirname "$0")/../.."

# The serving workload: `workerd serve` handling many requests, which the tests alone don't
# exercise. It keeps every function's entry in its profile (see build/pgo/profile-env.sh).
workload=//src/workerd/server/tests/pgo-workload:pgo-workload-test
# How much the workload's counts weigh against the tests'. The tests start workerd thousands of
# times and hand each Worker a few events, so unweighted, their startup code would outweigh the
# request path. Of 1, 10, 100 and the workload alone, 100 served the hello worlds 2-4% faster
# than 10, within the noise between rounds, but ran an HTMLRewriter Worker, which no training
# covers, in 3% more instructions. The workload alone was slowest on both Workers left out.
workload_weight=${PGO_WORKLOAD_WEIGHT:-10}

# Train on the tests that run the workerd binary, in every variant but @gc-stress, whose forced
# garbage collections production doesn't do. The others cover production paths that the default
# variant doesn't: the newest compatibility date, and the code behind autogates. Tests of other
# binaries would write no profile of workerd.
bazel query --output=label '
  let all = tests(//src/...) except attr(tags, "manual", tests(//src/...)) in
  attr(data, "//src/workerd/server:workerd\b", $all) except attr(name, "@gc-stress$", $all)' \
  > "$OUT/targets.txt"
grep -qxF "$workload" "$OUT/targets.txt" || echo "$workload" >> "$OUT/targets.txt"

# Failing tests still contribute the profiles they wrote, so a few failures don't stop training,
# though a failure of the workload does (below). The caller's flags come first so that these take
# precedence. A cached result is as good as a new one: its profiles are among the test's outputs,
# which Bazel downloads.
status=0
bazel test "$@" \
  --config=pgo-instrument --run_under=//build/pgo:profile-env \
  --test_env=PGO_DENSE_PROFILE_TEST="$workload" \
  --nozip_undeclared_test_outputs --remote_download_regex='.*test\.outputs/workerd.*\.profdata' \
  --keep_going --test_output=errors --skip_incompatible_explicit_targets \
  --target_pattern_file="$OUT/targets.txt" || status=$?
# 3: some tests failed. 4: no tests matched.
if [[ $status -ne 0 && $status -ne 3 ]]; then
  echo "bazel test failed with status $status" >&2
  exit $status
fi

# Collect the tests' merged profiles. A sharded test has a test.outputs directory per shard, and a
# retried one keeps its failed attempts' too.
testlogs=$(bazel info --config=pgo-instrument "$@" bazel-testlogs)
sed -E 's|^//||; s|:|/|' "$OUT/targets.txt" | while read -r test; do
  [[ ! -d "$testlogs/$test" ]] || find -L "$testlogs/$test" -path '*/test.outputs/workerd*.profdata'
done > "$OUT/profiles.txt"
dense=$(grep '/workerd\.dense\.profdata$' "$OUT/profiles.txt" | head -1 || true)
if [[ -z "$dense" ]]; then
  echo "$workload wrote no profile; it must pass to train" >&2
  exit 1
fi
echo "Merging $(wc -l < "$OUT/profiles.txt") profiles, the workload's with weight $workload_weight"
grep -vF "$dense" "$OUT/profiles.txt" | grep -v '\.dense\.profdata$' > "$OUT/test-profiles.txt" || true
# A process killed mid-write leaves a truncated profile; skip those rather than fail.
merge=(merge --failure-mode=warn -o "$OUT/workerd.profdata"
  --weighted-input="$workload_weight,$dense" -f "$OUT/test-profiles.txt")
if [[ -n "${LLVM_PROFDATA:-}" ]]; then
  "$LLVM_PROFDATA" "${merge[@]}"
else
  bazel run "$@" //build/pgo:llvm-profdata -- "${merge[@]}"
fi

zstd -19 -T0 -q -f "$OUT/workerd.profdata" -o "$OUT/workerd.profdata.zst"
echo "Wrote $OUT/workerd.profdata.zst"
