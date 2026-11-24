#!/bin/bash
# Test wrapper (`--run_under`) for PGO training (see build/pgo/README.md): points the instrumented
# workerd processes' raw profiles at the test's undeclared outputs directory, so that Bazel returns
# them even from remote executors, and merges them when the test ends.
#
# workerd exits through _exit(), which skips the profile runtime's exit handler that writes the
# profile, unless KJ_CLEAN_SHUTDOWN is set. WORKERD_PGO_TRAINING makes the serving workload
# (src/workerd/server/tests/pgo-workload) send enough requests to train on.
export KJ_CLEAN_SHUTDOWN=1
export WORKERD_PGO_TRAINING=1
export LLVM_PROFILE_FILE="$TEST_UNDECLARED_OUTPUTS_DIR/workerd-%p.profraw"

status=0
"$@" || status=$?

# Every raw profile holds a counter for each function in the binary, and a test can run workerd
# several times, so the raw profiles of all the tests don't fit on a CI runner's disk. Merge them
# into one that leaves out the functions the test didn't run.
#
# Those zero-count entries tell the compiler which functions training never ran, so that it treats
# them as cold, so one test, which train.sh names in PGO_DENSE_PROFILE_TEST, keeps them all.
llvm_profdata="$RUNFILES_DIR/@LLVM_PROFDATA@"
sparse=--sparse
out=workerd.profdata
if [[ "${TEST_TARGET:-}" == "${PGO_DENSE_PROFILE_TEST:-}" ]]; then
  sparse=""
  out=workerd.dense.profdata
fi
cd "$TEST_UNDECLARED_OUTPUTS_DIR"
shopt -s nullglob
raws=(workerd-*.profraw)
if [[ ${#raws[@]} -gt 0 ]]; then
  # A process killed mid-write leaves a truncated profile; skip those rather than fail.
  if "$llvm_profdata" merge $sparse --failure-mode=warn -o "$out" "${raws[@]}"; then
    rm -f "${raws[@]}"
  else
    # Fail the test, so that this doesn't go unnoticed until the runner's disk fills up.
    echo "profile-env: couldn't merge ${raws[*]}" >&2
    status=1
  fi
fi
exit $status
