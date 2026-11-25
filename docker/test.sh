#!/bin/sh
# Smoke-tests a workerd image built from Dockerfile:
#
#     docker/test.sh <image> <platform>
#
# <image> is a local image, or a reference to pull, such as <name>@sha256:<digest>. The script
# checks that the image runs as 65532:65532 and runs `workerd --version`. It then serves two
# one-module wrangler projects from /app, one with a wrangler.json and one with a wrangler.jsonc,
# with the image's own entrypoint and user, on a read-only root filesystem with no capabilities,
# and checks that each answers 200. Last, it serves a project with a Durable Object counter as
# README.md shows, with `--data-dir=/data` appended to the command line and a volume at /data the
# only writable path, and checks that the count goes on in a new container on the same volume.
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
volume=$container-data
app=
cleanup() {
  docker rm -f "$container" "$probe" > /dev/null 2>&1 || true
  docker volume rm -f "$volume" > /dev/null 2>&1 || true
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

fail() {
  echo "$1" >&2
  docker logs "$container" >&2
  exit 1
}

# Makes $app a project directory whose config is <config> (wrangler.json or wrangler.jsonc), with
# the JSON members <members> after the common ones, and whose src/index.js is read from stdin.
project() {
  app=$(mktemp -d "${TMPDIR:-/tmp}/workerd-image-test.XXXXXX")
  mkdir "$app/src"
  cat > "$app/$1" << EOF
{
  // Both files are read as JSONC.
  "name": "smoke-test",
  "main": "src/index.js",
  "compatibility_date": "2026-01-01",
  "no_bundle": true,
  $2
}
EOF
  cat > "$app/src/index.js"
  # The container's user is not the owner of the files.
  chmod -R a+rX "$app"
}

# Starts the image with the project $app as $container, on a read-only root filesystem with no
# capabilities, with the volume $data, if set, at /data, and with the arguments given appended to
# the image's command line.
start() {
  docker run -d --name "$container" --platform "$platform" --read-only \
    --cap-drop ALL --security-opt no-new-privileges ${data:+--volume "$data:/data"} \
    --volume "$app:/app:ro" --publish 127.0.0.1::8080 "$image" "$@" > /dev/null
  address=$(docker port "$container" 8080/tcp 2> /dev/null | head -n 1) || true
  if [ -z "$address" ]; then
    fail "The container exited before it published its port."
  fi
}

# Sets $body to the container's 200 answer to GET /. Up to 30 tries, a second apart, while the
# container runs.
get() {
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
}

# Serves a project whose config is /app/<config> and checks that it answers with the greeting in
# its config.
serve() {
  config=$1
  project "$config" "\"vars\": { \"GREETING\": \"hello from $config\" }," << 'EOF'
export default {
  fetch(request, env) {
    return new Response(`${env.GREETING}\n`);
  },
};
EOF
  data=
  start
  get
  if [ "$body" != "hello from $config" ]; then
    fail "$config: unexpected response: $body"
  fi
  echo "$image ($platform) served the project with a $config: $body"
  docker rm -f "$container" > /dev/null
  rm -rf "$app"
  app=
}

serve wrangler.json
serve wrangler.jsonc

# Each request adds one to the count that the one Counter object stores.
project wrangler.jsonc '"durable_objects": { "bindings": [{ "name": "COUNTER", "class_name": "Counter" }] },
  "migrations": [{ "tag": "v1", "new_sqlite_classes": ["Counter"] }],' << 'EOF'
import { DurableObject } from 'cloudflare:workers';

export class Counter extends DurableObject {
  fetch() {
    const count = (this.ctx.storage.kv.get('count') ?? 0) + 1;
    this.ctx.storage.kv.put('count', count);
    return new Response(`${count}\n`);
  }
}

export default {
  fetch(request, env) {
    return env.COUNTER.get(env.COUNTER.idFromName('count')).fetch(request);
  },
};
EOF
docker volume create "$volume" > /dev/null
data=$volume
for expected in 1 2; do
  start --data-dir=/data
  get
  if [ "$body" != "$expected" ]; then
    fail "--data-dir=/data: the count is $body, not $expected."
  fi
  docker rm -f "$container" > /dev/null
done
echo "$image ($platform) kept a Durable Object's count on a volume with --data-dir=/data"
