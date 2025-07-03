#!/usr/bin/env python3
"""
Script to update Node.js version in node-version.h based on GitHub releases.

This script:
1. Fetches Node.js version tags from GitHub
2. Finds the maximum even major version
3. Looks for the latest version in the (max-2) series
4. Updates the node-version.h file if needed
"""

import json
import re
import sys
import urllib.request
from pathlib import Path


def fetch_nodejs_tags():
    """Fetch Node.js version tags from GitHub API."""
    url = "https://api.github.com/repos/nodejs/node/tags"
    headers = {
        "User-Agent": "workerd-node-version-updater",
        "Accept": "application/vnd.github.v3+json",
    }

    all_tags = []
    page = 1

    while True:
        req = urllib.request.Request(f"{url}?page={page}&per_page=100", headers=headers)
        try:
            with urllib.request.urlopen(req) as response:
                data = json.loads(response.read().decode())
                if not data:
                    break
                all_tags.extend(data)
                page += 1
        except Exception as e:
            print(f"Error fetching tags: {e}", file=sys.stderr)
            sys.exit(1)

    return all_tags


def parse_version(version_str):
    """Parse a version string like 'v22.17.0' into (major, minor, patch)."""
    match = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", version_str)
    if match:
        return tuple(map(int, match.groups()))
    return None


def find_target_version(tags):
    """Find the target Node.js version based on the algorithm."""
    # Extract valid versions
    versions = []
    for tag in tags:
        version = parse_version(tag["name"])
        if version:
            versions.append((version, tag["name"]))

    if not versions:
        print("No valid versions found", file=sys.stderr)
        sys.exit(1)

    # Sort versions
    versions.sort(reverse=True)

    # Find maximum even major version
    max_even_major = None
    for (major, _minor, _patch), _ in versions:
        if major % 2 == 0:
            max_even_major = major
            break

    if max_even_major is None:
        print("No even major version found", file=sys.stderr)
        sys.exit(1)

    # Calculate target major version (max_even - 2)
    target_major = max_even_major - 2

    # Find latest version in target major series
    for (major, _minor, _patch), tag_name in versions:
        if major == target_major:
            return tag_name[1:]  # Remove 'v' prefix

    print(f"No version found for major version {target_major}", file=sys.stderr)
    sys.exit(1)


def update_header_file(file_path, new_version):
    """Update the node-version.h file with the new version."""
    path = Path(file_path)
    content = path.read_text()

    # Replace the version string
    # Match: static constexpr kj::StringPtr nodeVersion = "X.Y.Z"_kj;
    pattern = r'static constexpr kj::StringPtr nodeVersion = "[^"]+"_kj;'
    replacement = f'static constexpr kj::StringPtr nodeVersion = "{new_version}"_kj;'
    new_content = re.sub(pattern, replacement, content)

    if new_content != content:
        path.write_text(new_content)
        return True
    return False


def main():
    if len(sys.argv) != 2:
        print("Usage: update_node_version.py <output_file>", file=sys.stderr)
        sys.exit(1)

    output_file = sys.argv[1]

    print("Fetching Node.js versions from GitHub...")
    tags = fetch_nodejs_tags()

    print("Finding target version...")
    target_version = find_target_version(tags)
    print(f"Target version: {target_version}")

    # Update the header file
    if update_header_file(output_file, target_version):
        print(f"Updated {output_file} with version {target_version}")
    else:
        print(f"No changes needed - already at version {target_version}")


if __name__ == "__main__":
    main()
