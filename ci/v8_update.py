#!/usr/bin/env python3
"""Automate workerd's mechanical V8 patch rebase and pin updates."""

import argparse
import base64
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path
from urllib.request import urlopen

import jsonc

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

# rust_icu_sys selects its bindings and versioned ICU symbol names from this
# pin, which must match the major version of the ICU that V8 links.
RUST_ICU_VERSION_PINS = (
    ROOT / "build/deps/rust.MODULE.bazel",
    ROOT / "src/rust/i18n/BUILD.bazel",
)
RUST_ICU_VERSION = re.compile(r'(?<="RUST_ICU_MAJOR_VERSION_NUMBER": ")\d+(?=")')

# V8 builds these crates from Chromium's third_party/rust. Every crate in their
# runtime dependency trees must resolve to the version Chromium builds V8 with.
CHROMIUM_CRATE_ROOTS = ("temporal_capi",)
CARGO_TOML = ROOT / "deps/rust/Cargo.toml"


def run(args, *, cwd=ROOT, check=True, env=None, stdout=None):
    print("+", shlex.join(map(str, args)), flush=True)
    return subprocess.run(
        list(map(str, args)), cwd=cwd, check=check, text=True, env=env, stdout=stdout
    )


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


def _v8_dependency_commit(path, deps):
    match = re.search(rf"'{re.escape(path)}'\s*:.*?'([0-9a-f]{{40}})'", deps, re.DOTALL)
    return match.group(1)


def _v8_deps_at(tag):
    return run(
        ["git", "show", f"{tag}:DEPS"], cwd=CHECKOUT, stdout=subprocess.PIPE
    ).stdout


def changed_dependencies(old, target):
    """Return the V8_DEPENDENCIES whose V8 DEPS revision differs between two tags."""
    old_deps = _v8_deps_at(old)
    target_deps = _v8_deps_at(target)
    return tuple(
        name
        for name, (path, _) in V8_DEPENDENCIES.items()
        if _v8_dependency_commit(path, old_deps)
        != _v8_dependency_commit(path, target_deps)
    )


def _update_aligned_dependencies():
    doc = jsonc.loads(DEPS.read_text())
    repositories = {repo["name"]: repo for repo in doc.data["repositories"]}
    v8_deps = (CHECKOUT / "DEPS").read_text()
    for name, (path, aligned) in V8_DEPENDENCIES.items():
        if aligned:
            repositories[name]["freeze_commit"] = _v8_dependency_commit(path, v8_deps)
    DEPS.write_text(jsonc.dumps(doc) + "\n")


def _icu_major_version(commit):
    with urlopen(
        "https://chromium.googlesource.com/chromium/deps/icu/+/"
        f"{commit}/source/common/unicode/uvernum.h?format=TEXT",
        timeout=30,
    ) as response:
        header = base64.b64decode(response.read()).decode()
    match = re.search(
        r"(?<=^#define U_ICU_VERSION_MAJOR_NUM )\d+$", header, re.MULTILINE
    )
    return match.group()


def _update_rust_icu_version():
    path, _ = V8_DEPENDENCIES["com_googlesource_chromium_icu"]
    commit = _v8_dependency_commit(path, (CHECKOUT / "DEPS").read_text())
    version = _icu_major_version(commit)
    changed = False
    for pin in RUST_ICU_VERSION_PINS:
        text = pin.read_text()
        old = RUST_ICU_VERSION.findall(text)
        if len(old) != 1:
            raise ValueError(
                f"expected one RUST_ICU_MAJOR_VERSION_NUMBER in {pin}, found {len(old)}"
            )
        if old != [version]:
            pin.write_text(RUST_ICU_VERSION.sub(version, text))
            changed = True
    if changed:
        print(
            f"Updated RUST_ICU_MAJOR_VERSION_NUMBER to {version}. Check that the pinned "
            f"rust_icu_sys provides bindgen/lib_{version}.rs; update the crate if it does not."
        )


def _chromium_crate_versions(commit):
    """Return _reachable_versions() of Chromium's chromium_crates_io/Cargo.lock."""
    with urlopen(
        "https://chromium.googlesource.com/chromium/src/third_party/rust/+/"
        f"{commit}/chromium_crates_io/Cargo.lock?format=TEXT",
        timeout=30,
    ) as response:
        return _reachable_versions(
            tomllib.loads(base64.b64decode(response.read()).decode())
        )


def _reachable_versions(lock):
    """Return {name: [version, ...]} for the crates CHROMIUM_CRATE_ROOTS reach in `lock`.

    Lockfile dependency edges don't record their kind, so this also reaches proc
    macros and build dependencies. Only crates in _chromium_crate_tree() are looked
    up, and it excludes those.
    """
    packages = {
        (package["name"], package["version"]): package.get("dependencies", [])
        for package in lock["package"]
    }
    locked = {}
    for name, version in packages:
        locked.setdefault(name, []).append(version)

    def resolve(dependency):
        # Cargo writes "name" when the lockfile holds one version of the crate, and
        # "name version" or "name version (source)" when it holds several.
        name, *rest = dependency.split()
        if rest:
            return name, rest[0]
        (version,) = locked[name]
        return name, version

    pending = []
    for root in CHROMIUM_CRATE_ROOTS:
        versions = locked.get(root, [])
        if len(versions) != 1:
            raise ValueError(
                f"Chromium's Cargo.lock must lock exactly one version of {root}, "
                f"but locks {', '.join(versions) or 'none'}"
            )
        pending.append((root, versions[0]))
    reachable = set()
    while pending:
        package = pending.pop()
        if package not in reachable:
            reachable.add(package)
            pending.extend(map(resolve, packages[package]))

    versions = {}
    for name, version in sorted(reachable):
        versions.setdefault(name, []).append(version)
    return versions


def _chromium_version(chromium, name):
    """Return Chromium's version of a crate, or None if there isn't exactly one."""
    versions = chromium.get(name, [])
    return versions[0] if len(versions) == 1 else None


def _cargo(*args, check=True, stdout=None):
    # The cargo of the Rust toolchain Bazel builds with, not whichever is on PATH.
    cargo = ["bazel", "run", "@rules_rust//tools/upstream_wrapper:cargo", "--"]
    return run(
        [*cargo, *args, "--manifest-path", CARGO_TOML], check=check, stdout=stdout
    ).stdout


def _chromium_crate_tree():
    """Return the (name, version) runtime dependencies of CHROMIUM_CRATE_ROOTS."""
    args = ["tree", "--target=all", "--edges=normal,no-proc-macro"]
    for root in CHROMIUM_CRATE_ROOTS:
        args += ["--package", root]
    tree = _cargo(*args, "--prefix=none", "--format={p}", stdout=subprocess.PIPE)
    packages = (line.split()[:2] for line in tree.splitlines() if line)
    return {(name, version.removeprefix("v")) for name, version in packages}


def _exact_pins():
    """Return {name: version} for the dependencies that Cargo.toml pins with `=`."""
    dependencies = tomllib.loads(CARGO_TOML.read_text())["dependencies"]
    pins = {}
    for name, spec in dependencies.items():
        version = spec if isinstance(spec, str) else spec.get("version", "")
        if version.startswith("="):
            pins[name] = version.removeprefix("=")
    return pins


def _update_chromium_crates(v8_deps):
    commit = _v8_dependency_commit("third_party/rust", v8_deps)
    chromium = _chromium_crate_versions(commit)

    # Exact pins in Cargo.toml hold crates that Cargo would otherwise resolve
    # newer than Chromium's versions. Move them first, since moving a pinned crate
    # can move others along with it. `cargo add` keeps each dependency's features,
    # and moves all the pins in one resolution, since one pin's new version may
    # require another's.
    pins = _exact_pins()
    repins = []
    for name, version in sorted(_chromium_crate_tree()):
        target = _chromium_version(chromium, name)
        if pins.get(name) == version and target not in (None, version):
            repins.append(f"{name}@={target}")
    if repins:
        _cargo("add", *repins)

    # Cargo.lock alone holds the rest. If Cargo refuses a version, it prints why,
    # and the check below names the crate.
    for name, version in _chromium_crate_tree():
        target = _chromium_version(chromium, name)
        if target not in (None, version):
            _cargo("update", f"{name}@{version}", "--precise", target, check=False)

    mismatched = [
        f"{name} {version} (Chromium: {', '.join(chromium.get(name, [])) or 'none'})"
        for name, version in sorted(_chromium_crate_tree())
        if _chromium_version(chromium, name) != version
    ]
    if mismatched:
        raise ValueError(
            f"Crates differ from Chromium's third_party/rust at {commit}: "
            f"{', '.join(mismatched)}. If Cargo cannot downgrade a crate by itself, "
            f'pin it with `<crate> = {{ version = "=<version>" }}` in {CARGO_TOML}.'
        )

    # Bazel builds the crates from cargo-bazel-lock.json, which must be regenerated
    # after Cargo.toml or Cargo.lock changes (see build/deps/rust.MODULE.bazel).
    # Regenerate it even if this run changed neither, since an earlier run that
    # failed the check above, or a pin added by hand, may have.
    run(["bazel", "fetch", "--repo_env=CARGO_BAZEL_REPIN=1", "--repo=@crates_vendor"])


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
    run(["git", "init", CHECKOUT])
    run(
        ["git", "remote", "add", "origin", "https://github.com/v8/v8.git"],
        cwd=CHECKOUT,
    )
    for tag in (old, target):
        run(
            [
                "git",
                "fetch",
                "--depth=1",
                "origin",
                f"refs/tags/{tag}:refs/tags/{tag}",
            ],
            cwd=CHECKOUT,
        )
    run(["git", "checkout", "-B", "workerd-patches", old], cwd=CHECKOUT)

    patch_files = sorted(PATCHES.glob("*.patch"))

    git_env = os.environ | {
        "GIT_COMMITTER_NAME": "workerd V8 update",
        "GIT_COMMITTER_EMAIL": "ew-v8-patches@cloudflare.com",
    }

    # --keep-non-patch is the counterpart of the -k that finish_update passes to
    # format-patch. With no patch files, git am would read a patch from stdin.
    if patch_files:
        run(
            [
                "git",
                "am",
                "--keep-non-patch",
                "--3way",
                "--committer-date-is-author-date",
                *patch_files,
            ],
            cwd=CHECKOUT,
            env=git_env,
        )
    rebase = run(
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
    with tempfile.TemporaryDirectory() as patch_dir:
        run(
            [
                "git",
                "format-patch",
                "--full-index",
                "-k",
                "--no-signature",
                "--no-stat",
                "--zero-commit",
                "--output-directory",
                patch_dir,
                target,
            ],
            cwd=CHECKOUT,
        )
        generated = sorted(Path(patch_dir).glob("*.patch"))
        for patch in PATCHES.glob("*.patch"):
            patch.unlink()
        for patch in generated:
            # copy2 preserves the patch's timestamps and other file metadata.
            shutil.copy2(patch, PATCHES / patch.name)
        patch_names = [patch.name for patch in generated]

    _update_module(target, patch_names)
    _update_aligned_dependencies()
    # The Rust ICU pin must match the ICU that Bazel builds, so regenerate the
    # ICU repository from its new commit before deriving the pin.
    run(
        [
            sys.executable,
            ROOT / "build/deps/update-deps.py",
            "com_googlesource_chromium_icu",
        ]
    )
    _update_rust_icu_version()
    _update_chromium_crates((CHECKOUT / "DEPS").read_text())


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
