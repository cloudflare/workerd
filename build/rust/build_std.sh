#!/usr/bin/env bash

# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Builds a Rust standard library from source for rules_rust, instrumented by a sanitizer or not.
#
# Arguments:
#   RUST_TOOLCHAIN_ROOT  Toolchain containing rustc and Cargo.
#   RUST_LIBRARY         Root of Rust's library source tree.
#   TARGET_DIR           Cargo target directory.
#   OUTPUT_DIR           Directory for stable-named rlibs declared to Bazel.
#   WORK_DIR             Scratch directory for the generated Cargo package.
#   TARGET_TRIPLE        Rust target triple.
#   HOST_TRIPLE          Triple of the platform the build scripts run on (may equal TARGET_TRIPLE).
#   SANITIZER            rustc -Zsanitizer value (may be empty).
#   EXTRA_RUSTFLAGS      Space-separated additional rustc flags (may be empty).
#   LINKER               Linker for the build scripts, run from the execution root.
#   CRATE...             Standard-library crates declared as Bazel outputs.

set -euo pipefail

if [[ $# -lt 11 ]]; then
  echo "usage: $0 RUST_TOOLCHAIN_ROOT RUST_LIBRARY TARGET_DIR OUTPUT_DIR WORK_DIR TARGET_TRIPLE HOST_TRIPLE SANITIZER EXTRA_RUSTFLAGS LINKER CRATE..." >&2
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
host_triple=$7
sanitizer=$8
extra_rustflags=$9
linker=$(abs_path "${10}")
shift 10
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
# -Zbuild-std and -Zsanitizer are unstable. A nightly toolchain accepts them anyway, and a toolchain
# that is built from a nightly commit but marked stable, as Chromium's is, accepts them with this.
export RUSTC_BOOTSTRAP=1
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
cargo_target_env() { printf 'CARGO_TARGET_%s' "$(printf '%s' "$1" | tr '[:lower:]-' '[:upper:]_')"; }
target_env=$(cargo_target_env "$target_triple")
rustflags=${sanitizer:+-Zsanitizer=$sanitizer}
rustflags=${rustflags}${rustflags:+${extra_rustflags:+ }}${extra_rustflags}
export "${target_env}_RUSTFLAGS=$rustflags"
# The build scripts run here, on the host, and Cargo links them with this.
export "$(cargo_target_env "$host_triple")_LINKER=$linker"
BUILD_STD_EXECROOT=$(pwd -P)
export BUILD_STD_EXECROOT

"$rust_root/bin/cargo" build \
  --manifest-path "$work/Cargo.toml" \
  --release \
  --target "$target_triple" \
  -Zbuild-std=std,panic_unwind,test \
  -Zbuild-std-features=backtrace,panic-unwind

# Older Cargo writes the artifacts to deps/, newer Cargo to build/<package>/<hash>/out/.
release="$target_dir/$target_triple/release"
copy_unique_artifact() {
  local pattern=$1
  local destination=$2
  local sources=()
  if [[ -d "$release/build" ]]; then
    while IFS= read -r source; do
      sources+=("$source")
    done < <(find "$release/build" -mindepth 4 -maxdepth 4 -path "*/out/$pattern" -print)
  fi
  if [[ ${#sources[@]} -eq 0 && -d "$release/deps" ]]; then
    while IFS= read -r source; do
      sources+=("$source")
    done < <(find "$release/deps" -maxdepth 1 -name "$pattern" -print)
  fi
  if [[ ${#sources[@]} -ne 1 ]]; then
    echo "Cargo produced ${#sources[@]} files matching $pattern; expected exactly one" >&2
    if [[ ${#sources[@]} -gt 0 ]]; then
      printf '  %s\n' "${sources[@]}" >&2
    fi
    exit 1
  fi
  cp "${sources[0]}" "$destination"
}

# Newer Cargo builds the standard library with -Zembed-metadata=no, which leaves only a stub of the
# crate's metadata in its rlib; rustc reads the rest from the .rmeta of the same name.
for crate in "${crates[@]}"; do
  for extension in rlib rmeta; do
    copy_unique_artifact "lib$crate-*.$extension" "$output_dir/lib${crate}_${crate}.$extension"
  done
done
