#!/bin/bash
# Uploads a profile made by build/pgo/train.sh as an asset of the pgo-profiles GitHub release, and
# points build/pgo/profile.json at it (see build/pgo/README.md).
#
# Usage: build/pgo/upload.sh PROFILE
#
# Needs gh, authenticated with permission to write the repository's releases. The asset is named by
# the date and the commit the profile was trained on. An existing asset is never replaced, so the
# profile that a commit names stays the same.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

repo=${GITHUB_REPOSITORY:-cloudflare/workerd}
tag=pgo-profiles
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
name="workerd-$(date -u +%Y-%m-%d)-$(git -C "$(dirname "$0")" rev-parse --short=10 HEAD).profdata.zst"
cp "$1" "$dir/$name"

# One release holds every profile, so that the profiles don't crowd the workerd releases or change
# which release their notes start from. It's a prerelease, which GitHub never marks as the latest.
if ! gh release view "$tag" --repo "$repo" > /dev/null 2>&1; then
  gh release create "$tag" --repo "$repo" --prerelease --latest=false --title "PGO profiles" \
    --notes "The profiles that optimize the Linux release builds (build/pgo/README.md). Don't delete them: build/pgo/profile.json names one of them, as may older commits that are built again."
fi
gh release upload "$tag" "$dir/$name" --repo "$repo"

integrity="sha256-$(openssl dgst -sha256 -binary "$1" | openssl base64 -A)"
cat > "$(dirname "$0")/profile.json" <<JSON
{
  "url": "https://github.com/$repo/releases/download/$tag/$name",
  "integrity": "$integrity"
}
JSON
echo "Uploaded $name and pointed build/pgo/profile.json at it:"
cat "$(dirname "$0")/profile.json"
