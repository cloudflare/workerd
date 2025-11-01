#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail

# Get the most recent commit from the v2 branch
freeze_commit="${COMMIT:-$(git ls-remote https://github.com/capnproto/capnproto.git refs/heads/v2 | cut -f1)}"
tarball_url="https://github.com/capnproto/capnproto/tarball/${freeze_commit}"

# Calculate SHA256 of the tarball
if [ -z "${SHA256:-}" ]; then
  echo "Fetching tarball to calculate SHA256..."
  temp_file=$(mktemp)
  curl -sL "$tarball_url" -o "$temp_file"
  sha256=$(shasum -a 256 "$temp_file" | cut -d' ' -f1)
  rm "$temp_file"
  echo "Calculated SHA256: $sha256"
else
  sha256="$SHA256"
fi
target="//build/deps/cpp.MODULE.bazel:capnp-cpp"

buildozer "set url \"$tarball_url\"" $target
buildozer "set strip_prefix \"capnproto-capnproto-${freeze_commit:0:7}/c++\"" $target
buildozer "set sha256 \"${sha256}\"" $target
