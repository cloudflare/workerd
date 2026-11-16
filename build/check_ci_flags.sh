#!/usr/bin/env bash
# Checks that CI's flags build what a developer's do: CI may change how Bazel runs and reports a
# build, never an action, so that CI and workstations share cache entries. Bazel configures a test
# target under the developer's flags alone and under the developer's and CI's together, and the two
# configurations' options must match apart from those that can't change a remote action.
#
# Usage: build/check_ci_flags.sh "<developer flags>" <CI flags>...
set -euo pipefail

read -ra developer <<<"$1"
shift
# A test, since test options such as --test_env only reach test targets' configuration, with no
# dependencies to analyze.
probe=//build/fixtures:flags-probe

# Only symlink trees on the client depend on --build_runfile_links.
ignored='^  build_runfile_links: '

options() {
  local id
  id=$(bazel cquery "$@" "$probe" | awk '{ print $2 }' | tr -d '()')
  [[ -n "$id" ]] || { echo "error: no configuration for $probe under $*" >&2; exit 1; }
  bazel config "$id" | grep -v -e '^BuildConfigurationValue ' -e '^Skyframe Key: ' -e "$ignored"
}

# Bazel keeps only the latest analysis's configurations, so the two run one after the other.
developer_options=$(options "${developer[@]}")
ci_options=$(options "${developer[@]}" "$@")
if ! diff <(echo "$developer_options") <(echo "$ci_options"); then
  echo "error: these CI flags change the build: $*" >&2
  echo "Put flags that shape actions in the developer's config instead." >&2
  exit 1
fi
