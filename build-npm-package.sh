#! /bin/bash
# TODO(cleanup): Convert to bazel rules/GitHub Actions
set -euo pipefail

if [ -z "${1-}" ]; then
    echo "Please specify a command"
    exit 1
fi
bazel build @capnp-cpp//src/capnp:capnp_tool

export LATEST_COMPATIBILITY_DATE=$(bazel-bin/external/capnp-cpp/src/capnp/capnp_tool eval src/workerd/io/compatibility-date.capnp supportedCompatibilityDate)
export WORKERD_VERSION=1.$(echo $LATEST_COMPATIBILITY_DATE | tr -d '-' | tr -d '"').0

bazel_build() {
    bazel build -c opt //src/workerd/server:workerd
    mkdir -p $1/bin
    node npm/scripts/bump-version.mjs $1/package.json
    cp bazel-bin/src/workerd/server/workerd $1/bin/workerd
}

case $1 in
build-darwin-arm64)
    bazel_build npm/workerd-darwin-arm64
    ;;
publish-darwin-arm64)
    cd npm/workerd-darwin-arm64 && npm publish
    ;;
build-darwin)
    bazel_build npm/workerd-darwin-64
    ;;
publish-darwin)
    cd npm/workerd-darwin-64 && npm publish
    ;;
build-linux-arm64)
    bazel_build npm/workerd-linux-arm64
    ;;
publish-linux-arm64)
    cd npm/workerd-linux-arm64 && npm publish
    ;;
build-linux)
    bazel_build npm/workerd-linux-64
    ;;
publish-linux)
    cd npm/workerd-linux-64 && npm publish
    ;;
build-shim)
    node npm/scripts/bump-version.mjs npm/workerd/package.json
    mkdir -p npm/workerd/lib
    mkdir -p npm/workerd/bin
    npx esbuild npm/lib/node-install.ts --outfile=npm/workerd/install.js --bundle --target=node16 --define:LATEST_COMPATIBILITY_DATE=$LATEST_COMPATIBILITY_DATE --platform=node --external:workerd --log-level=warning
    npx esbuild npm/lib/node-shim.ts --outfile=npm/workerd/bin/workerd --bundle --target=node16 --define:LATEST_COMPATIBILITY_DATE=$LATEST_COMPATIBILITY_DATE --platform=node --external:workerd --log-level=warning
    npx esbuild npm/lib/node-path.ts --outfile=npm/workerd/lib/main.js --bundle --target=node16 --define:LATEST_COMPATIBILITY_DATE=$LATEST_COMPATIBILITY_DATE --platform=node --external:workerd --log-level=warning
    node npm/scripts/build-shim-package.mjs
    ;;
publish-shim)
    cd npm/workerd && npm publish
    ;;
clean)
    rm -f npm/workerd/install.js
    rm -rf npm/workerd-darwin-64/bin
    rm -rf npm/workerd-darwin-arm64/bin
    rm -rf npm/workerd-linux-64/bin
    rm -rf npm/workerd-linux-arm64/bin
    rm -rf npm/workerd/bin
    rm -rf npm/workerd/lib
    ;;
*)
    echo "Invalid command"
    exit 1
    ;;
esac
