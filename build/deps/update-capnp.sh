#!/usr/bin/env bash

set -o errexit -o nounset -o pipefail

freeze_commit="${COMMIT:-164284a476eee4bfbcf7cce2a9a82fb156504210}"
sha256="${SHA256:-03f11165d64c0941c66e749554d3eaf0ea29b6ebe445a82e21557dc68caf6e28}"
target="//build/deps/cpp.MODULE.bazel:capnp-cpp"

buildozer "set url \"https://github.com/capnproto/capnproto/tarball/${freeze_commit}\"" $target
buildozer "set strip_prefix \"capnproto-capnproto-${freeze_commit:0:7}/c++\"" $target
buildozer "set sha256 \"${sha256}\"" $target
