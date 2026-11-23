#!/usr/bin/env bash
# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Regenerates ../aws-chunked-sdk-fixtures.js: uploads the fixture payloads with each client to a
# local server that records the request bodies, then writes them out. Needs node, python3, uv, go
# and openssl, and network access to fetch the clients. See README.md.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
HTTP_PORT="${HTTP_PORT:-18812}"
HTTPS_PORT="${HTTPS_PORT:-18813}"
trap 'kill "${SERVER:-}" 2>/dev/null || true; rm -rf "$WORK"' EXIT
cd "$WORK"

openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 1 \
  -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" 2>/dev/null

mkdir js py go
# This release identifies itself as aws-sdk-js/3.1174.0 in its user agent, which the js-v3
# fixtures record.
(cd js && npm init -y >/dev/null && npm install --silent @aws-sdk/client-s3@3.1175.0)
cp "$HERE/js-v3-client.mjs" js/client.mjs
uv venv -q py/.venv
uv pip install -q --python py/.venv/bin/python boto3==1.44.9 botocore==1.44.9
cp "$HERE/go-client.go" go/main.go
(cd go && go mod init fixtures >/dev/null 2>&1 &&
  go get -t github.com/aws/aws-sdk-go-v2/service/s3@v1.116.0 \
    github.com/aws/aws-sdk-go-v2/credentials@v1.21.3 github.com/minio/minio-go/v7@v7.4.1 \
    2>/dev/null && go mod tidy 2>/dev/null && go build -o client .)

python3 "$HERE/capture.py" captures.json "$HTTP_PORT" "$HTTPS_PORT" cert.pem key.pem > capture.log 2>&1 &
SERVER=$!
until grep -q ready capture.log; do sleep 0.2; done

(cd js && node client.mjs "$HTTP_PORT" "$HTTPS_PORT" "$WORK/cert.pem")
py/.venv/bin/python "$HERE/boto3-client.py" "$HTTP_PORT" "$HTTPS_PORT" "$WORK/cert.pem"
go/client "$HTTP_PORT" "$HTTPS_PORT" "$WORK/cert.pem"

python3 "$HERE/to-js.py" captures.json > "$HERE/../aws-chunked-sdk-fixtures.js"
echo "Wrote $(grep -c "name:" "$HERE/../aws-chunked-sdk-fixtures.js") fixtures to aws-chunked-sdk-fixtures.js"
