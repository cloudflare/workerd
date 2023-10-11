#!/usr/bin/env bash

# This script is hooked into bazel via the --workspace_status_command in .bazelrc. It
# runs during each build.

script_dir=$(dirname "$0")
inside_work_tree=$(git rev-parse --is-inside-work-tree 2>/dev/null)

# Check for issues that may affect developer workspace

# Logs an issue
# $1 = description
function issue_detected() {
  let issue_count=issue_count+1
  cat >&2 <<EOF
Workspace issue detected #${issue_count}

$1

EOF
}

# Check githooks
GITHOOKS_MSG=$(cat<<EOF
  It looks like the git config option core.hooksPath is not set. The workerd
  repository uses hooks stored in githooks/ to check for common mistakes
  and prevent production breakages.

  To set up the hooks, please run:

    git config core.hooksPath githooks
EOF
)
if [ "${inside_work_tree}" = "true" ] && [ "$EUID" -ne 0 ] && [ -z "`git config core.hooksPath`" ]; then
  issue_detected "${GITHOOKS_MSG}"
fi
