#! /bin/bash
# simple wrapper script to execute clang-tidy

set -euo pipefail

CLANG_TIDY_BIN=$1
shift

OUTPUT=$1
shift

PWD=$(pwd)/
ESCAPED_PWD=$(sed 's/[\*\.&/]/\\&/g' <<< "$PWD")

# Interestingly clang-tidy prints real errors to stdout, but system message like
# `4 warnings generated` when they are filtered out, to stderr.
# Save stderr and print only on errors to reduce the clutter.
CLANG_TIDY_STDERR=$(mktemp)

set +e
"${CLANG_TIDY_BIN}" "$@" 2>"$CLANG_TIDY_STDERR" | \
  # clang-tidy insists on printing absolute file paths, chop current dir off
  sed "s/$ESCAPED_PWD//g"
CLANG_TIDY_EXIT_CODE=$?
set -e

if [[ $CLANG_TIDY_EXIT_CODE -ne 0 ]]; then
  cat "$CLANG_TIDY_STDERR" >&2
  rm -f "$CLANG_TIDY_STDERR"
  exit $CLANG_TIDY_EXIT_CODE
fi

rm -f "$CLANG_TIDY_STDERR"

# bazel needs run action to produce some output, touch the file
touch "$OUTPUT"
