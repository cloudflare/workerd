#!/bin/bash
# Link-truth check for the Rust I/O backend: inspects the symbols of the linked workerd binary
# and fails if kj's own C++ event-loop setup made it into the link.
#
# Under --//:io_backend=rust, kj::setupAsyncIo() is supplied by //src/workerd/util:setup-async-io
# (tokio-backed) by symbol override, and the concrete kj OS I/O layer (kj-async-os) must not be
# linked. The analysis-time gate (//src/workerd/server:rust-io-hermeticity) forbids dependency
# EDGES to it; this test checks the RESULT: when kj-async-os does get pulled in (e.g. through the
# @capnp-cpp//src/kj:kj-async umbrella), the static link picks whichever setupAsyncIo() it meets
# first, and a binary quietly running on kj::UnixEventPort passes every test that builds its own
# event loop. So assert on the linked symbols themselves:
#   * kj's setupAsyncIo() has a local class `BasicContext`, and kj's async-io-unix.c++ has
#     `LowLevelAsyncIoProviderImpl`; neither may be present.
#   * the shim's TU references kj_rs_io::TokioAsyncIoContext; it must be present.
# The kj::UnixEventPort::* symbols ARE expected: the shim defines an inert UnixEventPort because
# kj::AsyncIoContext names the type (see setup-async-io-tokio.c++).
#
# Unix only (target_compatible_with in the BUILD): the patterns are Itanium-mangled names, and
# MSVC mangles differently (`?setupAsyncIo@kj@@...`), so on Windows this would inspect nothing.
# Symbols are matched in their MANGLED form (Itanium ABI, identical for GNU/LLVM/Apple nm):
# demangling a debug-sized workerd binary takes minutes, the raw table seconds. `12setupAsyncIo`
# is the length-prefixed identifier inside kj::setupAsyncIo's mangled name, and so on.
set -euo pipefail

BIN="$1"
if command -v nm >/dev/null 2>&1; then NM=nm
elif command -v llvm-nm >/dev/null 2>&1; then NM=llvm-nm
else echo "rust-io-link-check: no nm/llvm-nm on PATH"; exit 1
fi

# Stream the (large, debug-build) symbol table to a file once; scan it with grep from there.
# -p: no sorting -- sorting a debug workerd's table is what made this take ~25 s on CI, past the
# 15 s CI cap for medium tests.
SYMS="${TEST_TMPDIR:-/tmp}/rust-io-link-check.syms"
"$NM" -p "$BIN" > "$SYMS"

kj_total=$(grep -c 'N2kj' "$SYMS" || true)
if [ "$kj_total" -eq 0 ]; then
  echo "rust-io-link-check: FAIL -- $BIN carries no kj:: symbols. This check needs an unstripped"
  echo "  binary (it is tagged no-asan because the ASAN lane builds with --strip=always); a"
  echo "  configuration that strips cannot be verified this way and must rely on the analysis-time"
  echo "  gate (//src/workerd/server:rust-io-hermeticity)."
  exit 1
fi

kj_setup=$(grep -c '12setupAsyncIo.*12BasicContext' "$SYMS" || true)         # kj::setupAsyncIo(...)::BasicContext
kj_lowlevel=$(grep -c '28LowLevelAsyncIoProviderImpl' "$SYMS" || true)        # kj::(anon)::LowLevelAsyncIoProviderImpl
shim=$(grep -c '8kj_rs_io19TokioAsyncIoContext' "$SYMS" || true)              # kj_rs_io::TokioAsyncIoContext
unixport=$(grep -c 'N2kj13UnixEventPort' "$SYMS" || true)                     # kj::UnixEventPort

echo "rust-io-link-check: $BIN"
echo "  kj setupAsyncIo()::BasicContext symbols : $kj_setup   (must be 0)"
echo "  kj LowLevelAsyncIoProviderImpl symbols  : $kj_lowlevel   (must be 0)"
echo "  shim kj_rs_io::TokioAsyncIoContext refs : $shim   (must be > 0)"
echo "  kj::UnixEventPort::* symbols            : $unixport   (informational; the shim's inert port)"

status=0
if [ "$kj_setup" -ne 0 ] || [ "$kj_lowlevel" -ne 0 ]; then
  echo "FAIL: kj's C++ setupAsyncIo()/OS I/O provider is linked into the rust-backend binary."
  echo "      Some dependency reaches @capnp-cpp//src/kj:kj-async-os (usually via the :kj-async"
  echo "      umbrella). Find every offending edge with:"
  echo "        bazel cquery --//:io_backend=rust 'rdeps(deps(//src/workerd/server:workerd), @capnp-cpp//src/kj:kj-async, 1)'"
  echo "      and retarget it to :kj-async-core / :kj-async-io."
  status=1
fi
if [ "$shim" -eq 0 ]; then
  echo "FAIL: the tokio setupAsyncIo() shim (//src/workerd/util:setup-async-io) is not in the link."
  status=1
fi
exit $status
