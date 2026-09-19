#!/bin/bash
# Dependency-graph check for the Rust I/O backend: under --//:io_backend=rust, the workerd binary
# must not reach
#   * kj-async-os (kj's own event loop and sockets), directly or through the
#     @capnp-cpp//src/kj:kj-async umbrella: kj::setupAsyncIo() and kj::UnixEventPort's members
#     would be defined twice -- by kj and by the tokio shim (//src/workerd/util:setup-async-io);
#   * kj-http-impl (kj's HTTP/1.1 implementation), directly or through the kj-http umbrella: its
#     kj:: symbols are defined over hyper by //src/workerd/util:kj-http;
#   * kj-tls (OpenSSL), replaced by rustls.
# With static archives the linker keeps whichever definition it meets first, silently.
#
# One query per forbidden target; empty output means clean, otherwise it prints one offending
# dependency path. Extra
# arguments are passed to bazel (CI passes its --config flags). The linked binary's symbols are
# checked separately by //src/workerd/server:rust-io-link-check.
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET=//src/workerd/server:workerd
FORBIDDEN=(
  "@capnp-cpp//src/kj:kj-async-os|Retarget the offending edge from the @capnp-cpp//src/kj:kj-async umbrella to :kj-async-core / :kj-async-io."
  "@capnp-cpp//src/kj/compat:kj-http-impl|Depend on //src/workerd/util:kj-http (or :kj-http-types) instead of @capnp-cpp//src/kj/compat:kj-http."
  "@capnp-cpp//src/kj/compat:kj-tls|Use //src/workerd/server:tls-network instead of kj-tls."
)

errlog=$(mktemp)
trap 'rm -f "$errlog"' EXIT
status=0
for entry in "${FORBIDDEN[@]}"; do
  forbidden=${entry%%|*}
  hint=${entry#*|}
  if ! paths=$(bazel cquery "$@" --//:io_backend=rust "somepath($TARGET, $forbidden)" 2>"$errlog"); then
    cat "$errlog" >&2
    exit 1
  fi
  if [ -n "$paths" ]; then
    echo "FAIL: $TARGET reaches $forbidden under --//:io_backend=rust:"
    echo "$paths"
    echo "$hint"
    status=1
  else
    echo "ok: $TARGET does not reach $forbidden under --//:io_backend=rust"
  fi
done
exit $status
