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

# Portable across GNU and BSD userlands (macOS ships bash 3.2 and lacks GNU realpath -m):
# existing directories resolve via pwd -P, not-yet-existing outputs via their (existing) parent.
abs_dir() { (cd "$1" && pwd -P); }
abs_path() { printf '%s/%s\n' "$(abs_dir "$(dirname "$1")")" "$(basename "$1")"; }

rust_root=$(abs_dir "$1")
rust_library=$(abs_dir "$2")
target_dir=$(abs_path "$3")
output_dir=$(abs_path "$4")
work=$(abs_path "$5")
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

touch "$work/src/lib.rs"

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
# Cargo also reads config.toml from every ancestor of the working directory, and Bazel's
# sandbox lives under the developer's home (~/Library/Caches/bazel on macOS, ~/.cache/bazel on
# Linux), so a developer's ~/.cargo/config.toml leaks into this action. Explicitly unset the
# settings that would redirect the compiler (an empty wrapper disables it); RUSTFLAGS and the
# linker are pinned below via the target-specific env var, which outranks config files.
export RUSTC_WRAPPER=
export RUSTC_WORKSPACE_WRAPPER=
export CARGO_NET_OFFLINE=true
export CARGO_TARGET_DIR="$target_dir"
# Cargo has no public override for rust-src's location. This test-only hook is
# what Cargo itself uses to exercise -Zbuild-std against a separate source tree.
export __CARGO_TESTS_ONLY_SRC_ROOT="$rust_library"

# Cargo applies target-specific flags to the generated package and every build-std dependency. The
# action only builds rlibs; sanitizer runtime selection is deferred to the final Bazel link, where
# Rust and C++ share Clang's runtime.
env_name="CARGO_TARGET_$(printf '%s' "$target_triple" | tr '[:lower:]-' '[:upper:]_')_RUSTFLAGS"
export "$env_name=-Zsanitizer=$sanitizer"

"$rust_root/bin/cargo" build \
  --manifest-path "$work/Cargo.toml" \
  --release \
  --target "$target_triple" \
  -Zbuild-std=std,panic_unwind,test \
  -Zbuild-std-features=backtrace,panic-unwind

deps="$target_dir/$target_triple/release/deps"
copy_unique_rlib() {
  local crate=$1
  local destination=$2
  local sources=()
  while IFS= read -r source; do
    sources+=("$source")
  done < <(find "$deps" -maxdepth 1 -name "lib$crate-*.rlib" -print)
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
