#! /bin/bash
set -euo pipefail

if [[ $(uname -m) == 'x86_64' ]]; then
  echo "This _must_ be run on an Apple Silicon machine, since the macOS ARM build cannot be dockerised due to macOS license restrictions"
  exit 1
fi

rm -f workerd-darwin-arm64 workerd-darwin-arm64.gz
rm -f workerd-linux-arm64 workerd-linux-arm64.gz

# Get the tag associated with the latest release, to ensure parity between binaries
TAG_NAME=$(curl -sL https://api.github.com/repos/cloudflare/workerd/releases/latest | jq -r ".tag_name")

git checkout $TAG_NAME

# Build macOS binary
pnpm exec bazelisk build --disk_cache=./.bazel-cache -c opt //src/workerd/server:workerd

cp bazel-bin/src/workerd/server/workerd ./workerd-darwin-arm64

docker buildx build --platform linux/arm64 -f Dockerfile.release -t workerd:$TAG_NAME --target=artifact --output type=local,dest=$(pwd) .

chmod +x workerd*

mv workerd-darwin-arm64 workerd
gzip -9N workerd
mv workerd.gz workerd-darwin-arm64.gz

mv workerd-linux-arm64 workerd
gzip -9N workerd
mv workerd.gz workerd-linux-arm64.gz
