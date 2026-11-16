#!/bin/sh
# Smoke-tests a workerd image built from Dockerfile:
#
#     docker/test.sh <image> <platform>
#
# <image> is a local image, or a reference to pull, such as <name>@sha256:<digest>. The script
# checks that the image runs as 65532:65532 and runs `workerd --version`. It then serves a
# one-module wrangler project from /app with the image's own entrypoint and user, on a read-only
# root filesystem with no capabilities, and checks that it answers 200.
set -eu

if [ $# -ne 2 ]; then
  echo "usage: $0 <image> <platform>" >&2
  exit 2
fi
image=$1
platform=$2

# Named up front, so that cleanup finds them even if the script stops while `docker` starts them.
container=workerd-image-test-$$
probe=$container-probe
app=
cleanup() {
  docker rm -f "$container" "$probe" > /dev/null 2>&1 || true
  if [ -n "$app" ]; then
    rm -rf "$app"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A container created from the image, which pulls it if needed, has the user of the image for
# that platform, even where the reference names a multi-platform index.
docker create --quiet --name "$probe" --platform "$platform" "$image" > /dev/null
user=$(docker container inspect --format '{{.Config.User}}' "$probe")
docker rm "$probe" > /dev/null
if [ "$user" != 65532:65532 ]; then
  echo "$image runs as \"$user\", not 65532:65532" >&2
  exit 1
fi
docker run --rm --platform "$platform" --entrypoint /usr/local/bin/workerd "$image" --version

app=$(mktemp -d "${TMPDIR:-/tmp}/workerd-image-test.XXXXXX")
mkdir "$app/src"
cat > "$app/wrangler.json" << 'EOF'
{
  "name": "smoke-test",
  "main": "src/index.js",
  "compatibility_date": "2026-01-01",
  "no_bundle": true
}
EOF
cat > "$app/src/index.js" << 'EOF'
export default {
  fetch() {
    return new Response("hello from workerd\n");
  },
};
EOF
# The container's user is not the owner of the files.
chmod -R a+rX "$app"

docker run -d --name "$container" --platform "$platform" --read-only \
  --cap-drop ALL --security-opt no-new-privileges \
  --volume "$app:/app:ro" --publish 127.0.0.1::8080 "$image" > /dev/null
fail() {
  echo "$1" >&2
  docker logs "$container" >&2
  exit 1
}
address=$(docker port "$container" 8080/tcp 2> /dev/null | head -n 1) || true
if [ -z "$address" ]; then
  fail "The container exited before it published its port."
fi

# Up to 30 tries, a second apart, while the container runs.
tries=0
until body=$(curl --fail --silent --show-error --max-time 5 "http://$address/" 2> /dev/null); do
  tries=$((tries + 1))
  if [ "$(docker inspect --format '{{.State.Running}}' "$container")" != true ]; then
    fail "The container exited before it answered."
  fi
  if [ "$tries" -ge 30 ]; then
    fail "The container did not answer http://$address/ with a 200."
  fi
  sleep 1
done
if [ "$body" != "hello from workerd" ]; then
  fail "unexpected response: $body"
fi
echo "$image ($platform) served the project: $body"
