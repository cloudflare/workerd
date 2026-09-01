#!/usr/bin/env bash

# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Builds a sanitizer-instrumented Rust standard library for rules_rust.
#
# Arguments:
#   RUST_TOOLCHAIN_ROOT  Toolchain containing rustc and Cargo.
#   RUST_LIBRARY         Root of Rust's library source tree.
#   TARGET_DIR           Cargo target directory.
#   OUTPUT_DIR           Directory for stable-named rlibs declared to Bazel.
#   WORK_DIR             Scratch directory for the generated Cargo package.
#   TARGET_TRIPLE        Rust target triple.
#   SANITIZER            rustc -Zsanitizer value.
#   CRATE...             Standard-library crates declared as Bazel outputs.

set -euo pipefail

if [[ $# -lt 9 ]]; then
  echo "usage: $0 RUST_TOOLCHAIN_ROOT RUST_LIBRARY TARGET_DIR OUTPUT_DIR WORK_DIR TARGET_TRIPLE SANITIZER CRATE..." >&2
  exit 2
fi

rust_root=$(realpath "$1")
rust_library=$(realpath "$2")
target_dir=$(realpath -m "$3")
output_dir=$(realpath -m "$4")
work=$(realpath -m "$5")
target_triple=$6
sanitizer=$7
shift 7
crates=("$@")

rm -rf "$target_dir" "$output_dir" "$work"
mkdir -p "$work/src" "$work/cargo-home" "$output_dir"

cat > "$work/Cargo.toml" <<'EOF'
[package]
name = "workerd-build-std"
version = "0.0.0"
edition = "2024"

[profile.release]
debug = true
EOF

cat > "$work/src/lib.rs" <<'EOF'
#[test]
fn build_std() {}
EOF

# rust-src includes the exact third-party sources selected by the standard
# library's lockfile. Offline mode keeps this action hermetic.
cat > "$work/cargo-home/config.toml" <<EOF
[source.crates-io]
replace-with = "vendored-sources"
[source.vendored-sources]
directory = "$rust_library/vendor"
EOF

export PATH="$rust_root/bin:$PATH"
export RUSTC="$rust_root/bin/rustc"
export CARGO_HOME="$work/cargo-home"
export CARGO_NET_OFFLINE=true
export CARGO_TARGET_DIR="$target_dir"
# Cargo has no public override for rust-src's location. This test-only hook is
# what Cargo itself uses to exercise -Zbuild-std against a separate source tree.
export __CARGO_TESTS_ONLY_SRC_ROOT="$rust_library"

# Cargo applies target-specific flags to the generated package and every
# build-std dependency. The temporary test binary may use Rust's sanitizer
# runtime; Bazel-built targets use -Zexternal-clangrt to share Clang's runtime
# with C++ at their final link.
env_name="CARGO_TARGET_${target_triple^^}_RUSTFLAGS"
env_name=${env_name//-/_}
export "$env_name=-Zsanitizer=$sanitizer -Zexternal-clangrt -Clinker=clang -Clink-arg=-fsanitize=$sanitizer"

"$rust_root/bin/cargo" test \
  --manifest-path "$work/Cargo.toml" \
  --no-run \
  --release \
  --target "$target_triple" \
  -Zbuild-std=std,panic_unwind,test \
  -Zbuild-std-features=backtrace,panic-unwind

deps="$target_dir/$target_triple/release/deps"
copy_unique_rlib() {
  local crate=$1
  local destination=$2
  local sources=()
  mapfile -t sources < <(find "$deps" -maxdepth 1 -name "lib$crate-*.rlib" -print)
  if [[ ${#sources[@]} -ne 1 ]]; then
    echo "Cargo produced ${#sources[@]} rlibs for $crate; expected exactly one" >&2
    printf '  %s\n' "${sources[@]}" >&2
    exit 1
  fi
  cp "${sources[0]}" "$destination"
}

for crate in "${crates[@]}"; do
  copy_unique_rlib "$crate" "$output_dir/lib${crate}_${crate}.rlib"
done
