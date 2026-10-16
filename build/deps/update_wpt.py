#!/usr/bin/env python3
"""
Usage: update_wpt.py [--latest | VERSION]

Pins the wpt dependency to VERSION, a wpt release tag of the workerd-tools
repository, or by default to the newest such release. That repository also
holds releases of several other tools, so the plain "latest release" is usually
not one of ours. With --latest, prints the newest release's tag instead of
pinning it.
"""

import argparse
import importlib
import json
import re

import jsonc

# The hyphen in the file name keeps this out of reach of the import statement.
update_deps = importlib.import_module("update-deps")

TITLE = re.compile(r"wpt-.*")
PER_PAGE = 100


def all_releases(repo):
    """Every release of `repo`, over as many pages as it takes.

    The whole list is needed because the endpoint offers no way to sort it, and
    returns releases by `created_at`, which GitHub documents as the date of the
    commit a release was cut from rather than the date of the release. So the
    newest release is not necessarily on the first page.
    """
    owner, name = repo["owner"], repo["repo"]
    releases = []
    while True:
        page = json.loads(
            update_deps.github_urlopen(
                f"https://api.github.com/repos/{owner}/{name}/releases"
                f"?per_page={PER_PAGE}&page={len(releases) // PER_PAGE + 1}"
            ).read()
        )
        releases += page
        if len(page) < PER_PAGE:
            return releases


def matching_releases(repo):
    """The releases of `repo` titled like a wpt release, newest published first.

    Publication date is the only field that orders these releases: dozens of
    them share one `created_at`. A draft has no publication date, and is not a
    usable release anyway.
    """
    return sorted(
        (
            release
            for release in all_releases(repo)
            # GitHub shows a release with no title of its own under its tag name.
            if not release["draft"]
            and TITLE.fullmatch(release["name"] or release["tag_name"])
        ),
        key=lambda release: release["published_at"],
        reverse=True,
    )


def latest_release(repo):
    """The tag of the newest wpt release of `repo`."""
    releases = matching_releases(repo)
    if not releases:
        raise LookupError(
            f"No release of {repo['owner']}/{repo['repo']} is titled like {TITLE.pattern}"
        )
    return releases[0]["tag_name"]


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--latest", action="store_true", help="print the newest release's tag"
    )
    group.add_argument("version", nargs="?", help="the release tag to pin")
    return parser.parse_args()


def main():
    args = parse_args()
    update_deps.GITHUB_ACCESS_TOKEN = update_deps.read_access_token()

    deps_path = update_deps.SCRIPT_DIR / "shared_deps.jsonc"
    deps = jsonc.loads(deps_path.read_text())
    repo = next(r for r in deps.data["repositories"] if r["name"] == "wpt")

    if args.latest:
        print(latest_release(repo))
        return

    version = args.version or latest_release(repo)
    current = repo.get("freeze_version")
    if version == current:
        print(f"wpt is already at {current}")
        return

    print(f"Updating wpt {current} -> {version}")
    repo["freeze_version"] = version
    deps_path.write_text(jsonc.dumps(deps) + "\n")

    # Regenerate this one dependency, which downloads the release to record its
    # hash and prefix, and leaves every other dependency as it was.
    update_deps.TARGET_FILTER = "wpt"
    update_deps.process_config("shared_deps.jsonc")


if __name__ == "__main__":
    main()
