#!/usr/bin/env python3
"""
Script to update the pinned opencode version in Bonk CI workflow files.

This script:
1. Fetches the latest opencode-ai version from the npm registry
2. Scans all workflow files in .github/workflows/ for opencode_version pins
3. Updates them to the latest version
"""

import json
import re
import sys
import urllib.request
from pathlib import Path

WORKFLOW_DIR = Path(__file__).resolve().parent.parent / ".github" / "workflows"

# Matches opencode_version: 'X.Y.Z' or opencode_version: "X.Y.Z"
OPENCODE_VERSION_RE = re.compile(
    r"""(opencode_version:\s*)(["'])([^"']*)\2""",
)


def fetch_latest_version():
    """Fetch the latest opencode-ai version from the npm registry."""
    url = "https://registry.npmjs.org/opencode-ai/latest"
    headers = {
        "User-Agent": "workerd-opencode-version-updater",
        "Accept": "application/json",
    }

    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req) as response:
            data = json.loads(response.read().decode())
            return data["version"]
    except Exception as e:
        print(f"Error fetching latest opencode-ai version: {e}", file=sys.stderr)
        sys.exit(1)


def update_workflow_file(path, new_version):
    """Update the opencode_version field in a workflow file.

    Returns True if the file was changed.
    """
    content = path.read_text()

    # Replace the version while preserving the original quote style.
    new_content = OPENCODE_VERSION_RE.sub(
        rf"\g<1>\g<2>{new_version}\2",
        content,
    )

    if new_content != content:
        path.write_text(new_content)
        return True
    return False


def find_workflow_files():
    """Find all workflow files that contain an opencode_version pin."""
    return [
        path
        for path in sorted(WORKFLOW_DIR.glob("*.yml"))
        if OPENCODE_VERSION_RE.search(path.read_text())
    ]


def main():
    print("Fetching latest opencode-ai version from npm...")
    latest = fetch_latest_version()
    print(f"Latest version: {latest}")

    workflow_files = find_workflow_files()
    if not workflow_files:
        print("No workflow files with opencode_version pins found", file=sys.stderr)
        sys.exit(1)

    changed = False
    for path in workflow_files:
        relpath = path.relative_to(WORKFLOW_DIR.parent.parent)
        if update_workflow_file(path, latest):
            print(f"Updated {relpath} to {latest}")
            changed = True
        else:
            print(f"No changes needed in {relpath}")

    if not changed:
        print(f"Already at latest version ({latest})")


if __name__ == "__main__":
    main()
