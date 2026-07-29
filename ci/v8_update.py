#!/usr/bin/env python3
"""Automate workerd's mechanical V8 patch rebase and pin updates."""

import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path
from urllib.request import urlopen

import jsonc
import v8_nightly_shared

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = Path("/tmp/workerd-v8/v8")
MODULE = ROOT / "build/deps/v8.MODULE.bazel"
DEPS = ROOT / "build/deps/deps.jsonc"
PATCHES = ROOT / "patches/v8"

V8_DEPENDENCIES = {
    "com_googlesource_chromium_icu": ("third_party/icu", True),
    "dragonbox": ("third_party/dragonbox/src", True),
    "fast_float": ("third_party/fast_float/src", True),
    "fp16": ("third_party/fp16/src", True),
    "highway": ("third_party/highway/src", True),
    "perfetto": ("third_party/perfetto", False),
    "simdutf": ("third_party/simdutf", False),
}
# Keep the aligned markers in sync with the corresponding comments in
# build/deps/deps.jsonc.


v8_nightly_shared.init(ROOT)


def latest_beta_v8():
    with urlopen(
        "https://chromiumdash.appspot.com/fetch_releases?platform=Win32&channel=beta",
        timeout=30,
    ) as response:
        releases = json.load(response)
    with urlopen(
        f"https://chromiumdash.appspot.com/fetch_version?version={releases[0]['version']}",
        timeout=30,
    ) as response:
        tag = json.load(response)["v8_version"]
    return tag


def read_version():
    match = re.search(r'^VERSION = "([^"]+)"$', MODULE.read_text(), re.MULTILINE)
    return match.group(1)


def _tarball_integrity(tag):
    with urlopen(
        f"https://github.com/v8/v8/archive/refs/tags/{tag}.tar.gz", timeout=120
    ) as response:
        digest = hashlib.sha256(response.read()).digest()
    return "sha256-" + base64.b64encode(digest).decode()


def _v8_dependency_commit(path, deps=None):
    deps = deps if deps is not None else (CHECKOUT / "DEPS").read_text()
    match = re.search(rf"'{re.escape(path)}'\s*:.*?'([0-9a-f]{{40}})'", deps, re.DOTALL)
    return match.group(1)


def changed_dependencies(old, target):
    old_deps = v8_nightly_shared.output(["git", "show", f"{old}:DEPS"], cwd=CHECKOUT)
    target_deps = v8_nightly_shared.output(
        ["git", "show", f"{target}:DEPS"], cwd=CHECKOUT
    )
    return tuple(
        name
        for name, (path, _) in V8_DEPENDENCIES.items()
        if _v8_dependency_commit(path, old_deps)
        != _v8_dependency_commit(path, target_deps)
    )


def _update_aligned_dependencies():
    doc = jsonc.loads(DEPS.read_text())
    repositories = {repo["name"]: repo for repo in doc.data["repositories"]}
    for name, (path, aligned) in V8_DEPENDENCIES.items():
        if aligned:
            repositories[name]["freeze_commit"] = _v8_dependency_commit(path)
    DEPS.write_text(jsonc.dumps(doc) + "\n")


def _update_module(tag, patch_names):
    text = MODULE.read_text()
    text = re.sub(
        r'^VERSION = "[^"]+"$',
        f'VERSION = "{tag}"',
        text,
        count=1,
        flags=re.MULTILINE,
    )
    text = re.sub(
        r'^INTEGRITY = "[^"]+"$',
        f'INTEGRITY = "{_tarball_integrity(tag)}"',
        text,
        count=1,
        flags=re.MULTILINE,
    )
    patch_list = (
        "PATCHES = [\n" + "".join(f'    "{name}",\n' for name in patch_names) + "]"
    )
    text = re.sub(
        r"^PATCHES = \[\n.*?^\]$",
        patch_list,
        text,
        count=1,
        flags=re.MULTILINE | re.DOTALL,
    )
    MODULE.write_text(text)


def prepare_update(target):
    old = read_version()
    if target == old:
        return True

    shutil.rmtree(CHECKOUT, ignore_errors=True)
    CHECKOUT.parent.mkdir(parents=True, exist_ok=True)
    v8_nightly_shared.run(["git", "init", CHECKOUT])
    v8_nightly_shared.run(
        ["git", "remote", "add", "origin", "https://github.com/v8/v8.git"],
        cwd=CHECKOUT,
    )
    for tag in (old, target):
        v8_nightly_shared.run(
            [
                "git",
                "fetch",
                "--depth=1",
                "origin",
                f"refs/tags/{tag}:refs/tags/{tag}",
            ],
            cwd=CHECKOUT,
        )
    v8_nightly_shared.run(
        ["git", "checkout", "-B", "workerd-patches", old], cwd=CHECKOUT
    )

    patch_files = sorted(PATCHES.glob("*.patch"))

    git_env = os.environ | {
        "GIT_COMMITTER_NAME": "workerd V8 nightly",
        "GIT_COMMITTER_EMAIL": "ew-v8-patches@cloudflare.com",
    }

    v8_nightly_shared.run(
        [
            "git",
            "am",
            "--3way",
            "--committer-date-is-author-date",
            *patch_files,
        ],
        cwd=CHECKOUT,
        env=git_env,
    )
    rebase = v8_nightly_shared.run(
        ["git", "rebase", "--onto", target, old, "workerd-patches"],
        cwd=CHECKOUT,
        check=False,
        env=git_env,
    )
    if rebase.returncode:
        print(f"Resolve the rebase in {CHECKOUT}, then run:")
        print(f"  {sys.executable} ci/v8_update.py finish {target}")
        return False
    finish_update(target)
    return True


def finish_update(target):
    with tempfile.TemporaryDirectory() as output:
        v8_nightly_shared.run(
            [
                "git",
                "format-patch",
                "--full-index",
                "-k",
                "--no-signature",
                "--no-stat",
                "--zero-commit",
                "--output-directory",
                output,
                target,
            ],
            cwd=CHECKOUT,
        )
        generated = sorted(Path(output).glob("*.patch"))
        for patch in PATCHES.glob("*.patch"):
            patch.unlink()
        for patch in generated:
            # copy2 preserves the patch's timestamps and other file metadata.
            shutil.copy2(patch, PATCHES / patch.name)
        patch_names = [patch.name for patch in generated]

    _update_module(target, patch_names)
    _update_aligned_dependencies()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Automate workerd's mechanical V8 patch rebase and pin updates."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser(
        "check-update", help="check Chrome Beta for a V8 update"
    )
    check.add_argument("--machine-readable", action="store_true")
    for name in ("update", "finish"):
        command = commands.add_parser(name)
        command.add_argument("version", metavar="VERSION")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.command == "check-update":
        current = read_version()
        target = latest_beta_v8()
        if current == target:
            return 0
        print(target if args.machine_readable else f"{current} -> {target}")
        return 1
    if args.command == "update":
        return 0 if prepare_update(args.version) else 1
    if args.command == "finish":
        finish_update(args.version)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
