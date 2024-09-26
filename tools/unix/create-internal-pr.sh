#!/usr/bin/env bash
# This tool will open a PR to update workerd in Cloudflare's internal repository
# to match the current branch.

set -euo pipefail

if [ -z "$EDGEWORKER_HOME" ]; then
	EDGEWORKER_HOME="$(git rev-parse --show-toplevel)/../edgeworker"
fi

BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
SYNC_BRANCH_NAME=sync/$BRANCH_NAME

cd $EDGEWORKER_HOME
git fetch origin master

if git rev-parse --verify $SYNC_BRANCH_NAME; then
	BRANCH_EXISTS=1
else
	BRANCH_EXISTS=0
fi


if [ $BRANCH_EXISTS -eq 1 ]; then
	git checkout $SYNC_BRANCH_NAME
else
	git checkout -b $SYNC_BRANCH_NAME origin/master
fi

git -C deps/workerd fetch origin $BRANCH_NAME
git -C deps/workerd checkout origin/$BRANCH_NAME
git add deps/workerd

if [ $BRANCH_EXISTS -eq 1 ]; then
	git commit --no-verify --amend
	git push --no-verify --force origin HEAD
else
	git commit --no-verify -m "Sync with $BRANCH_NAME"
	GIT_OUT="$(git push --no-verify origin HEAD 2>&1)"
	echo "$GIT_OUT"
	open $(echo "$GIT_OUT" | sed -n '/remote: *http/ s/remote: *\(.*\)/\1/p')
fi
