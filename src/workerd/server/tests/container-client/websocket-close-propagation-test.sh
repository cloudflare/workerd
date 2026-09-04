#!/usr/bin/env bash
# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

WORKERD_BINARY="$1"
CLIENT_BINARY="$2"
TEMPLATE_CAPNP="$3"
WORKER_JS="$4"

TMPDIR="${TEST_TMPDIR:-$(mktemp -d)}"
RUNDIR="$TMPDIR/ws-close-propagation"
mkdir -p "$RUNDIR"

cleanup() {
  if [[ -n "${WORKERD_PID:-}" ]]; then
    kill "$WORKERD_PID" 2>/dev/null || true
    wait "$WORKERD_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cp "$WORKER_JS" "$RUNDIR/websocket-close-propagation-worker.js"

LOG="$RUNDIR/workerd.log"

for attempt in $(seq 1 30); do
  PORT=$(( (RANDOM % 20000) + 30000 ))
  sed -e "s/__PORT__/${PORT}/g" \
    "$TEMPLATE_CAPNP" > "$RUNDIR/config.capnp"

  "$WORKERD_BINARY" serve "$RUNDIR/config.capnp" --experimental --verbose \
    --directory-path=TEST_TMPDIR="$TMPDIR" \
    >"$LOG" 2>&1 &
  WORKERD_PID=$!

  sleep 0.5
  if ! kill -0 "$WORKERD_PID" 2>/dev/null; then
    wait "$WORKERD_PID" 2>/dev/null || true
    unset WORKERD_PID
    continue
  fi

  URL="ws://127.0.0.1:${PORT}/ws"
  if "$CLIENT_BINARY" "$URL"; then
    exit 0
  fi

  echo "WebSocket close propagation test failed (${URL})" >&2
  echo "--- workerd log tail ---" >&2
  tail -200 "$LOG" >&2 || true
  exit 1
done

echo "failed to start workerd after repeated attempts" >&2
echo "--- workerd log tail ---" >&2
tail -200 "$LOG" >&2 || true
exit 1
