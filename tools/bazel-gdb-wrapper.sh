#!/bin/bash
#
# A wrapper script so workerd can be debugged with gdb, if invoked via bazel run.
#
# Example command-line invocation:
#
# bazel run -c dbg --run_under $(realpath tools/bazel-gdb-wrapper.sh) \
#   //src/workerd/server:workerd -- serve samples/helloworld_esm/config.capnp
#

set -euo pipefail

cd "${BUILD_WORKSPACE_DIRECTORY}"
gdb --args "$@"
