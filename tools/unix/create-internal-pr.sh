#!/bin/sh
# This tool will open a PR to update workerd in Cloudflare's internal repository
# to match the current branch.

set -euo pipefail

BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
SYNC_BRANCH_NAME=sync/$BRANCH_NAME

cd ../edgeworker
git fetch origin master

if git rev-parse --verify $SYNC_BRANCH_NAME; then
	BRANCH_EXISTS=1
else 
	BRANCH_EXISTS=0
fi


if [ $BRANCH_EXISTS -eq 1 ]; then
	git checkout $SYNC_BRANCH_NAME
else
	git checkout master 
	git checkout -b $SYNC_BRANCH_NAME
fi

git -C deps/workerd fetch origin $BRANCH_NAME
git -C deps/workerd checkout origin/$BRANCH_NAME
git add deps/workerd

if [ $BRANCH_EXISTS -eq 1 ]; then
	git commit --no-verify --amend
	git push --no-verify --force origin HEAD
else
	git commit --no-verify -m "Sync with $BRANCH_NAME"
	git push --no-verify origin HEAD 
	open "https://bitbucket.cfdata.org/projects/EW/repos/edgeworker/pull-requests?create&sourceBranch=refs/heads/$SYNC_BRANCH_NAME"
fi
