#!/bin/bash
# Dependency-graph check for workerd's tokio I/O layer: the workerd binary must not reach
# kj-async-os (kj's own event loop and sockets), directly or through the
# @capnp-cpp//src/kj:kj-async umbrella. If it does, kj::setupAsyncIo() and kj::UnixEventPort's
# members are defined twice -- by kj and by the tokio shim (//src/workerd/util:setup-async-io) --
# and with static archives the linker keeps whichever it meets first, silently.
#
# One query, empty output means clean; otherwise it prints one offending dependency path. Extra
# arguments are passed to bazel (CI passes its --config flags). The linked binary's symbols are
# checked separately by //src/workerd/server:rust-io-link-check.
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET=//src/workerd/server:workerd
FORBIDDEN=@capnp-cpp//src/kj:kj-async-os

errlog=$(mktemp)
trap 'rm -f "$errlog"' EXIT
if ! paths=$(bazel cquery "$@" "somepath($TARGET, $FORBIDDEN)" 2>"$errlog"); then
  cat "$errlog" >&2
  exit 1
fi
if [ -n "$paths" ]; then
  echo "FAIL: $TARGET reaches $FORBIDDEN:"
  echo "$paths"
  echo "Retarget the offending edge from the @capnp-cpp//src/kj:kj-async umbrella to :kj-async-core / :kj-async-io."
  exit 1
fi
echo "ok: $TARGET does not reach $FORBIDDEN"
