#!/usr/bin/env bash

set -euo pipefail

BAZEL_TARGET="$1"
REPO_ROOT="$(git rev-parse --show-toplevel)"

# Sloppily convert the Bazel target to a FS path
TEST_PATH="$REPO_ROOT/$(echo $BAZEL_TARGET | sed s_:_/_g | sed s_//__)"
TEST_BASENAME=$(basename $TEST_PATH)
TEST_DIRNAME=$(dirname $TEST_PATH)

cat << EOF > $TEST_PATH.wd-test
using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    ( name = "$TEST_BASENAME",
      worker = (
        modules = [
          (name = "worker", esModule = embed "$TEST_BASENAME.js")
        ],
       compatibilityFlags = ["nodejs_compat_v2"],
      )
    ),
  ],
);
EOF

git add $TEST_PATH.wd-test

cat << EOF > $TEST_PATH.js
export const test = {
  test() {
    // ...
  },
};
EOF

git add $TEST_PATH.js

cat << EOF >> $TEST_DIRNAME/BUILD.bazel

wd_test(
    src = "$TEST_BASENAME.wd-test",
    args = ["--experimental"],
    data = ["$TEST_BASENAME.js"],
)
EOF

git add $TEST_DIRNAME/BUILD.bazel
