#!/usr/bin/env python3
"""Probe Chrome Beta's V8 in workerd, then hand the candidate to edgeworker."""

from pathlib import Path

import v8_nightly_shared
from v8_update import (
    CHECKOUT,
    changed_dependencies,
    latest_beta_v8,
    prepare_update,
    read_version,
)

ROOT = Path(__file__).resolve().parents[1]
v8_nightly_shared.init(ROOT)
MAX_AI_FIXES = 3

BAZEL_CI_ARGS = [
    "-k",
    "--config=ci",
    "--config=ci-limit-storage",
    "--config=ci-linux-common",
    "--config=ci-test",
    "--announce_rc",
    "--remote_cache=",
]


def bazel_build():
    return (
        v8_nightly_shared.logged(
            ["bazel", "build", *BAZEL_CI_ARGS, "//src/workerd/server:workerd"],
            v8_nightly_shared.BUILD_LOG,
        )
        == 0
    )


def bazel_test():
    return (
        v8_nightly_shared.logged(
            ["bazel", "test", *BAZEL_CI_ARGS, "--test_output=errors", f"--build_event_json_file={v8_nightly_shared.BEP}", "//...",],
            v8_nightly_shared.TEST_LOG,
        )
        == 0
    )


def version_key(tag):
    return tuple(map(int, tag.split(".")))


def repair(mode, check, old_tag, target_tag):
    for _ in range(MAX_AI_FIXES):
        if check():
            return True
        if not v8_nightly_shared.ai("workerd", mode, old_tag, target_tag, CHECKOUT):
            return False

        v8_nightly_shared.commit(
            f"[v8-nightly] AI fix for V8 {target_tag}"
        )

    print(f"Workerd V8 nightly {mode} is still broken; AI fix budget exhausted")
    return check()


def main():
    v8_nightly_shared.clean_artifacts()

    v8_nightly_shared.setup_git("workerd")

    v8_nightly_shared.run(["git", "checkout", "-B", v8_nightly_shared.BRANCH, "HEAD"])
    old_tag = read_version()
    target_tag = latest_beta_v8()
    if version_key(target_tag) < version_key(old_tag):
        print(f"Keeping newer workerd V8 {old_tag}; Chrome Beta uses {target_tag}")
        target_tag = old_tag

    if old_tag == target_tag:
        v8_nightly_shared.commit(f"[v8-nightly] V8 {target_tag} unchanged")
        print(f"Workerd V8 {target_tag} unchanged")
        return 0

    if not prepare_update(target_tag) and not v8_nightly_shared.ai(
        "workerd", "rebase", old_tag, target_tag, CHECKOUT
    ):
        return 1

    for dependency in changed_dependencies(old_tag, target_tag):
        v8_nightly_shared.run(["python3", "build/deps/update-deps.py", dependency])

    v8_nightly_shared.commit(
        f"[v8-nightly] AI guided update for V8 {old_tag} -> {target_tag}"
    )

    if not repair("build", bazel_build, old_tag, target_tag):
        return 1

    if not repair("test", bazel_test, old_tag, target_tag):
        return 1

    print(f"Workerd V8 nightly: {old_tag} -> {target_tag} passes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
