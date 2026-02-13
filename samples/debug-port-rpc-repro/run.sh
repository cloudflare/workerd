#!/bin/bash
# Test whether workerdDebugPort RPC works with different patterns:
#   1. DirectProxy  - explicit ping() method on WorkerEntrypoint
#   2. ProxyProxy   - Proxy constructor pattern (like miniflare's rpc-proxy)
#   3. ProxyWithProps - Proxy constructor with props (like miniflare's ExternalServiceProxy)
#
# Requires workerd >= 1.20260213.0. Set WORKERD_BIN to override.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
WORKERD_BIN="${WORKERD_BIN:-workerd}"

lsof -ti:8080 -ti:8081 -ti:9230 2>/dev/null | xargs kill -9 2>/dev/null || true
sleep 0.5

echo "Starting target worker (port 8081, debug port 9230)..."
$WORKERD_BIN serve "$DIR/config-target.capnp" --experimental --debug-port=127.0.0.1:9230 &
sleep 2

echo "Starting caller worker (port 8080)..."
$WORKERD_BIN serve "$DIR/config-caller.capnp" --experimental &
sleep 2

echo ""
echo "=== Fetch tests ==="
echo "direct-fetch: $(curl -s 'http://localhost:8080/?test=direct-fetch')"
echo "proxy-fetch:  $(curl -s 'http://localhost:8080/?test=proxy-fetch')"
echo "props-fetch:  $(curl -s 'http://localhost:8080/?test=props-fetch')"

echo ""
echo "=== RPC tests ==="
echo "direct-rpc: $(curl -s 'http://localhost:8080/?test=direct-rpc')"
echo "proxy-rpc:  $(curl -s 'http://localhost:8080/?test=proxy-rpc')"
echo "props-rpc:  $(curl -s 'http://localhost:8080/?test=props-rpc')"

echo ""
kill %1 %2 2>/dev/null; wait 2>/dev/null
