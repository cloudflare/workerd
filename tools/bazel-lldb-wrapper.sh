#!/bin/bash
#
# A wrapper script so workerd can be debugged with lldb, if invoked via bazel run.
#
# Example command-line invocation:
#
# bazel run -c dbg --spawn_strategy=local --features=oso_prefix_is_pwd --run_under \
#   $(realpath tools/bazel-lldb-wrapper.sh) \
#   //src/workerd/server:workerd -- serve samples/helloworld_esm/config.capnp
#
# NB the additional spawn_strategy and features flags are necessary on OS X per
# https://github.com/bazelbuild/bazel/issues/6327.

set -euo pipefail

cd "${BUILD_WORKSPACE_DIRECTORY}"
lldb -- "$@"
