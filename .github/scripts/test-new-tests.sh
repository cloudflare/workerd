#!/bin/bash
# Reruns the tests introduced by the change under test with half their usual timeout. A new test
# that only just fits its budget on the day it lands will flake once CI runners are busy or slower
# variants (debug, ASan) catch up with it, so it has to demonstrate headroom up front.
#
# Usage: test-new-tests.sh <bazel test args...>
#
# NEW_TEST_BASE_SHA names the commit to diff against. Nothing happens when it is unset, which is
# the case for pushes to main.
#
# Only tests whose names the kj_test and wd_test macros derive from the source file name are
# covered: pkg/foo-test.c++ -> //pkg:foo-test@*, pkg/foo.wd-test -> //pkg:foo@*. Rust tests, Python
# tests and tests with an explicit name= are not.
set -euo pipefail

base="${NEW_TEST_BASE_SHA:-}"
if [ -z "$base" ]; then
  echo "NEW_TEST_BASE_SHA is not set; not checking new tests for timeout headroom."
  exit 0
fi

# CI checkouts are shallow, so the base commit is usually not present yet. Diffing two commits only
# needs their trees, not the history between them.
if ! git cat-file -e "$base^{commit}" 2>/dev/null; then
  git fetch --quiet --depth=1 origin "$base"
fi

# Nearest enclosing directory with a BUILD file.
package_of() {
  local dir=$1
  while [ "$dir" != "." ]; do
    if [ -e "$dir/BUILD.bazel" ] || [ -e "$dir/BUILD" ]; then
      echo "$dir"
      return
    fi
    dir=$(dirname "$dir")
  done
}

universe=()
prefixes=()
while IFS= read -r file; do
  pkg=$(package_of "$(dirname "$file")")
  [ -n "$pkg" ] || continue
  name=${file#"$pkg/"}
  name=${name%.c++}
  name=${name%.wd-test}
  name=${name%.ts-wd-test}
  universe+=("//$pkg:all")
  prefixes+=("//$pkg:$name@")
done < <(git diff --diff-filter=A --name-only "$base" HEAD -- '*-test.c++' '*.wd-test' '*.ts-wd-test')

if [ ${#prefixes[@]} -eq 0 ]; then
  echo "No new test sources since $base."
  exit 0
fi

# Resolve the name prefixes against the tests that actually exist in those packages, picking up
# every generated variant (@, @all-compat-flags, @all-autogates, ...) except the lint target.
targets=()
while IFS= read -r target; do
  [[ "$target" == *@eslint ]] && continue
  for prefix in "${prefixes[@]}"; do
    if [[ "$target" == "$prefix"* ]]; then
      targets+=("$target")
      break
    fi
  done
done < <(bazel query --noshow_progress "tests(set(${universe[*]}))" | tr -d '\r')

if [ ${#targets[@]} -eq 0 ]; then
  echo "No test targets found for new test sources: ${prefixes[*]}"
  exit 0
fi

# Bazel offers no way to read back the effective --test_timeout for a set of configs, but
# --announce_rc prints every rc option it applies in order, so the last one printed is the one in
# effect for this lane.
timeouts=$(bazel test --nobuild --announce_rc "$@" "${targets[0]}" 2>&1 |
  grep -o -- '--test_timeout=[0-9,]*' | tail -1 | cut -d= -f2 | tr -d '\r' || true)
if [ -z "$timeouts" ]; then
  echo "Could not determine the effective --test_timeout; not checking new tests."
  exit 0
fi
halved=$(echo "$timeouts" | tr ',' '\n' | awk '{ printf "%s%d", (NR > 1 ? "," : ""), int(($1 + 1) / 2) }')

echo "New tests must pass with --test_timeout=$halved (half of $timeouts):"
printf '  %s\n' "${targets[@]}"

if ! bazel test "$@" --test_timeout="$halved" "${targets[@]}"; then
  echo "::error::A new test failed with half its usual timeout ($halved instead of $timeouts)." \
    "Tests that only just fit their budget flake later; speed the test up or give it a larger size=."
  exit 1
fi
