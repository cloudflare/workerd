#!/usr/bin/env python3
"""
Script to update Node.js versions in node-version.h based on GitHub releases.

node-version.h has one `nodeVersion<major>` constant per reported major version.
This script:
1. Fetches Node.js version tags from GitHub
2. Finds the latest release in each major series named by a constant
3. Updates those constants in the node-version.h file if needed

Pass --major to update only one major version. Adding a new major version is a
manual change, since reporting it requires a compatibility flag.
"""

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

CONSTANT_PATTERN = re.compile(
    r'static constexpr kj::StringPtr nodeVersion(\d+) = "([^"]+)"_kj;'
)


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


def find_latest_version(tags, major):
    """Find the latest Node.js release in the given major series."""
    versions = [
        version
        for version in (parse_version(tag["name"]) for tag in tags)
        if version and version[0] == major
    ]

    if not versions:
        print(f"No version found for major version {major}", file=sys.stderr)
        sys.exit(1)

    return "{}.{}.{}".format(*max(versions))


def read_majors(file_path):
    """Return the major versions that have a constant in the header file."""
    content = Path(file_path).read_text()
    return [int(major) for major, _ in CONSTANT_PATTERN.findall(content)]


def update_header_file(file_path, major, new_version):
    """Update the constant for the given major version in the header file."""
    path = Path(file_path)
    content = path.read_text()

    # Match: static constexpr kj::StringPtr nodeVersion<major> = "X.Y.Z"_kj;
    pattern = rf'(static constexpr kj::StringPtr nodeVersion{major} = )"[^"]+"(_kj;)'
    new_content = re.sub(pattern, rf'\g<1>"{new_version}"\g<2>', content)

    if new_content != content:
        path.write_text(new_content)
        return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_file", help="path to node-version.h")
    parser.add_argument(
        "--major",
        type=int,
        help="update only this major version (default: every major in the file)",
    )
    args = parser.parse_args()

    majors = read_majors(args.output_file)
    if not majors:
        print(f"No nodeVersion constants found in {args.output_file}", file=sys.stderr)
        sys.exit(1)

    if args.major is not None:
        if args.major not in majors:
            print(
                f"{args.output_file} has no nodeVersion{args.major} constant. "
                "Reporting a new major version requires a compatibility flag, "
                "so add the constant by hand.",
                file=sys.stderr,
            )
            sys.exit(1)
        majors = [args.major]

    print("Fetching Node.js versions from GitHub...")
    tags = fetch_nodejs_tags()

    for major in majors:
        target_version = find_latest_version(tags, major)
        print(f"Target version for Node.js {major}: {target_version}")

        if update_header_file(args.output_file, major, target_version):
            print(f"Updated {args.output_file} with version {target_version}")
        else:
            print(f"No changes needed - already at version {target_version}")


if __name__ == "__main__":
    main()
