#!/bin/bash
# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Measures the send worker boundary with wrk: the same toy on the KJ loop, as a send worker
# behind the adapter, and behind a forwarding send worker or JavaScript Worker, all served by one
# `workerd-with-toys` process (see main.rs and config.capnp).
#
#   bazel build --config=opt //src/workerd/server/tests/send-worker:workerd-with-toys
#   src/workerd/server/tests/send-worker/bench.sh \
#       bazel-bin/src/workerd/server/tests/send-worker/workerd-with-toys
#
# Environment: DURATION (seconds per measurement, default 10), CONNECTIONS (default 32),
# SEND_WORKER_THREADS (the send worker runtime's size, passed through), BASE_PORT (default 18300).
#
# Prints one line per measurement: hello requests per second with the median and 99th percentile
# latency, then download and upload throughput in MiB/s.

set -euo pipefail

binary=$(realpath "${1:?usage: bench.sh <workerd-with-toys>}")
here=$(cd "$(dirname "$0")" && pwd)
duration=${DURATION:-10}
connections=${CONNECTIONS:-32}
base_port=${BASE_PORT:-18300}

sockets=(kj-toy send-toy send-hello send-fetch send-fetch-copy js-hello js-fetch
  js-fetch-send-toy js-fetch-send-fetch)
declare -A port
args=()
for i in "${!sockets[@]}"; do
  port[${sockets[$i]}]=$((base_port + i))
  args+=(--socket-addr "${sockets[$i]}=127.0.0.1:$((base_port + i))")
done

scratch=$(mktemp -d)
"$binary" serve "$here/config.capnp" "${args[@]}" >"$scratch/server.log" 2>&1 &
server=$!
trap 'kill -TERM $server 2>/dev/null; wait $server 2>/dev/null; rm -rf "$scratch"' EXIT

for _ in $(seq 100); do
  curl -s -o /dev/null "http://127.0.0.1:${port[js-fetch-send-fetch]}/" && break
  sleep 0.1
done

# The upload: a 64 MiB POST per request.
upload_mib=64
cat >"$scratch/upload.lua" <<EOF
wrk.method = "POST"
wrk.body = string.rep("x", $upload_mib * 1024 * 1024)
EOF

# The download: 256 MiB per request.
download_bytes=$((256 * 1024 * 1024))

field() { awk -v key="$1" '$1 == key { print $2 }' "$scratch/wrk.out"; }

# wrk prints rates with a unit suffix; this prints MiB.
mib() {
  awk '{
    value = $1 + 0; unit = $1; gsub(/[0-9.]/, "", unit)
    if (unit == "GB") value *= 1024; else if (unit == "KB") value /= 1024
    else if (unit == "B") value /= 1048576
    printf "%.0f", value
  }' <<<"$1"
}

hello() {
  wrk -t2 -c"$connections" -d"${duration}s" --latency "http://127.0.0.1:${port[$1]}/" \
    >"$scratch/wrk.out"
  printf '%-22s %10s req/s   p50 %-9s p99 %-9s\n' "$1" "$(field Requests/sec:)" \
    "$(field 50%)" "$(field 99%)"
}

download() {
  wrk -t1 -c2 -d"${duration}s" "http://127.0.0.1:${port[$1]}/bytes/$download_bytes" \
    >"$scratch/wrk.out"
  printf '%-22s %10s MiB/s\n' "$1" "$(mib "$(field Transfer/sec:)")"
}

upload() {
  wrk -t1 -c2 -d"${duration}s" -s "$scratch/upload.lua" "http://127.0.0.1:${port[$1]}/" \
    >"$scratch/wrk.out"
  printf '%-22s %10s MiB/s\n' "$1" \
    "$(awk -v mib=$upload_mib '$1 == "Requests/sec:" { printf "%.0f", $2 * mib }' \
      "$scratch/wrk.out")"
}

rss() { awk '/VmRSS/ { printf "%d MiB", $2 / 1024 }' "/proc/$server/status"; }

echo "# hello ($connections connections, ${duration}s each)"
for socket in kj-toy send-hello send-toy js-hello send-fetch send-fetch-copy js-fetch \
  js-fetch-send-toy js-fetch-send-fetch; do
  hello "$socket"
done
echo "# download, $((download_bytes / 1048576)) MiB responses (2 connections); RSS before: $(rss)"
for socket in kj-toy send-toy send-fetch send-fetch-copy js-fetch; do
  download "$socket"
done
echo "# upload, $upload_mib MiB requests (2 connections); RSS before: $(rss)"
for socket in kj-toy send-toy send-fetch send-fetch-copy js-fetch; do
  upload "$socket"
done
echo "# RSS after: $(rss)"
