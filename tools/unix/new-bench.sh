#!/usr/bin/env bash

# Scaffolds a wd_bench(): a config, a JavaScript module with a bench() handler, and the BUILD rule.

set -euo pipefail

BAZEL_TARGET="$1"
REPO_ROOT="$(git rev-parse --show-toplevel)"

# Sloppily convert the Bazel target to a FS path
BENCH_PATH="$REPO_ROOT/$(echo $BAZEL_TARGET | sed s_:_/_g | sed s_//__)"
BENCH_BASENAME=$(basename $BENCH_PATH)
BENCH_DIRNAME=$(dirname $BENCH_PATH)
BUILD_FILE="$BENCH_DIRNAME/BUILD.bazel"

cat << EOF > $BENCH_PATH.wd-bench
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "$BENCH_BASENAME",
      worker = (
        modules = [
          (name = "worker", esModule = embed "$BENCH_BASENAME.js")
        ],
        compatibilityFlags = ["nodejs_compat"],
      )
    ),
  ],
);
EOF

git add $BENCH_PATH.wd-bench

cat << EOF > $BENCH_PATH.js
// Copyright (c) $(date +%Y) Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  bench(b) {
    b.run('example', () => b.blackBox(JSON.stringify({ a: 1 })));
  },
};
EOF

git add $BENCH_PATH.js

cat << EOF >> $BUILD_FILE

wd_bench(
    src = "$BENCH_BASENAME.wd-bench",
    data = ["$BENCH_BASENAME.js"],
)
EOF

git add $BUILD_FILE

if ! grep -q '"//:build/wd_bench.bzl"' $BUILD_FILE; then
  echo "Add this to the loads at the top of $BUILD_FILE:"
  echo '  load("//:build/wd_bench.bzl", "wd_bench")'
fi
