#! /bin/bash
set -euo pipefail

if [[ $(uname -m) == 'x86_64' ]]; then
  echo "This _must_ be run on an Apple Silicon machine, since the macOS ARM build cannot be dockerised due to macOS license restrictions"
  exit 1
fi

rm -f workerd-darwin-arm64
rm -f workerd-linux-arm64

# Get the tag associated with the latest release, to ensure parity between binaries
TAG_NAME=$(curl -sL https://api.github.com/repos/cloudflare/workerd/releases/latest | jq -r ".tag_name")

git checkout $TAG_NAME

# Build macOS binary
#
# This is using fastbuild rather than opt until SIGBUS issue with the opt binary are resolved.
# The issue is tracked in https://github.com/cloudflare/workers-sdk/issues/2386.
pnpm exec bazelisk build -c fastbuild //src/workerd/server:workerd
echo Stripping binary
strip bazel-bin/src/workerd/server/workerd -o ./workerd-darwin-arm64

docker buildx build --platform linux/arm64 -f Dockerfile.release -t workerd:$TAG_NAME --target=artifact --output type=local,dest=$(pwd) .

chmod +x workerd*

mv workerd-darwin-arm64 workerd
gzip -9N workerd
mv workerd.gz workerd-darwin-arm64.gz

mv workerd-linux-arm64 workerd
gzip -9N workerd
mv workerd.gz workerd-linux-arm64.gz
