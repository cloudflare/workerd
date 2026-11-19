#!/bin/bash
# Usage: pgo-config-test.sh BAZELRC PROFILE_EXECPATH
set -euo pipefail
expected=$(grep -o -- '-Cprofile-use=[^ ]*' "$1" | cut -d= -f2-)
if [[ "$expected" != "$2" ]]; then
  echo "$1 passes rustc -Cprofile-use=$expected, but the profile is at $2" >&2
  exit 1
fi
