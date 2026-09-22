#!/bin/bash
# Link-truth check for the Rust I/O backend: inspects the symbol names of the linked workerd
# binary and fails if kj's own C++ event-loop setup made it into the link.
#
# kj::setupAsyncIo() is supplied by //src/workerd/util:setup-async-io
# (tokio-backed) by symbol override, and the concrete kj OS I/O layer (kj-async-os) must not be
# linked. The dependency-graph check (build/rust_io_graph_check.sh) forbids dependency EDGES to it;
# this test checks the RESULT: when kj-async-os does get pulled in (e.g. through the
# @capnp-cpp//src/kj:kj-async umbrella), the static link picks whichever setupAsyncIo() it meets
# first, and a binary quietly running on kj::UnixEventPort passes every test that builds its own
# event loop. So assert on the linked symbols themselves:
#   * kj's setupAsyncIo() has a local class `BasicContext`, and kj's async-io-unix.c++ has
#     `LowLevelAsyncIoProviderImpl`; neither may be present.
#   * the shim's TU references kj_rs_io::TokioAsyncIoContext; it must be present.
# The kj::UnixEventPort::* symbols ARE expected: the shim defines an inert UnixEventPort because
# kj::AsyncIoContext names the type (see setup-async-io.c++).
#
# How: the symbol names are matched in their MANGLED form (Itanium ABI, identical for GNU/LLVM/
# Apple toolchains) as strings of the binary itself, in one `grep -a` pass. In an unstripped
# binary every linked symbol's mangled name sits NUL-terminated in the string table (plus DWARF
# linkage names and RTTI type names), and none of these names can appear unless its defining
# translation unit was linked. This deliberately does not use nm: no nm is hermetically available
# to the test -- the C++ toolchain's is autoconfigured on the host that runs bazel and need not
# exist on a remote executor, and the Rust toolchain does not export its llvm-nm -- whereas
# grep is a given wherever this bash script runs. `12setupAsyncIo` is the length-prefixed
# identifier inside kj::setupAsyncIo's mangled name, and so on.
#
# Unix only (target_compatible_with in the BUILD): MSVC mangles differently
# (`?setupAsyncIo@kj@@...`), so on Windows this would inspect nothing.
set -euo pipefail

# $1 is `$(rlocationpath :workerd)`: relative to the runfiles root, whatever repository workerd
# is built from (`_main/...` standalone, `+local_repository+workerd/...` as a dependency).
BIN="${TEST_SRCDIR:-.}/$1"
[ -f "$BIN" ] || BIN="$1"
[ -f "$BIN" ] || { echo "rust-io-link-check: workerd binary not found: $1"; exit 1; }

PAT_KJ_SETUP='12setupAsyncIoEvEN?12BasicContext'      # kj::setupAsyncIo()::BasicContext (+ members)
PAT_KJ_LOWLEVEL='28LowLevelAsyncIoProviderImpl'       # kj::(anon)::LowLevelAsyncIoProviderImpl
PAT_SHIM='8kj_rs_io19TokioAsyncIoContext'             # kj_rs_io::TokioAsyncIoContext
PAT_UNIXPORT='N2kj13UnixEventPort'                    # kj::UnixEventPort
PAT_ANY_KJ='N2kj'                                     # any kj:: symbol at all

# One pass over the (large, debug-build) binary, extracting just the matching names; count
# from that small file. LC_ALL=C: byte-wise matching, no multibyte decoding of binary data.
SYMS="${TEST_TMPDIR:-/tmp}/rust-io-link-check.syms"
{ LC_ALL=C grep -a -o -E "$PAT_KJ_SETUP|$PAT_KJ_LOWLEVEL|$PAT_SHIM|$PAT_UNIXPORT|$PAT_ANY_KJ" "$BIN" || true; } > "$SYMS"
count() { { grep -c -E "$1" "$SYMS" || true; } | tr -d ' '; }

kj_total=$(count "$PAT_ANY_KJ")
if [ "$kj_total" -eq 0 ]; then
  echo "rust-io-link-check: FAIL -- $BIN carries no kj:: symbol names. This check needs an"
  echo "  unstripped binary (it is tagged no-asan because the ASAN lane builds with --strip=always);"
  echo "  a configuration that strips cannot be verified this way and must rely on the"
  echo "  dependency-graph check (build/rust_io_graph_check.sh)."
  exit 1
fi

kj_setup=$(count "$PAT_KJ_SETUP")
kj_lowlevel=$(count "$PAT_KJ_LOWLEVEL")
shim=$(count "$PAT_SHIM")
unixport=$(count "$PAT_UNIXPORT")

echo "rust-io-link-check: $BIN"
echo "  kj setupAsyncIo()::BasicContext names : $kj_setup   (must be 0)"
echo "  kj LowLevelAsyncIoProviderImpl names  : $kj_lowlevel   (must be 0)"
echo "  shim kj_rs_io::TokioAsyncIoContext    : $shim   (must be > 0)"
echo "  kj::UnixEventPort::* names            : $unixport   (informational; the shim's inert port)"

status=0
if [ "$kj_setup" -ne 0 ] || [ "$kj_lowlevel" -ne 0 ]; then
  echo "FAIL: kj's C++ setupAsyncIo()/OS I/O provider is linked into the workerd binary."
  echo "      Some dependency reaches @capnp-cpp//src/kj:kj-async-os (usually via the :kj-async"
  echo "      umbrella). Find every offending edge with:"
  echo "        bazel cquery 'rdeps(deps(//src/workerd/server:workerd), @capnp-cpp//src/kj:kj-async, 1)'"
  echo "      and retarget it to :kj-async-core / :kj-async-io."
  status=1
fi
if [ "$shim" -eq 0 ]; then
  echo "FAIL: the tokio setupAsyncIo() shim (//src/workerd/util:setup-async-io) is not in the link."
  status=1
fi
exit $status
