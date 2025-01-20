#!/usr/bin/env bash

BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)

gh workflow run 105611493 \
    --field pyodide=$1 \
    --field pyodideRevision=$2 \
    --field backport=$3 \
    -r $BRANCH_NAME
