#!/usr/bin/env python3
"""Shared infrastructure for workerd and edgeworker V8 nightly probes."""

import json
import os
import shlex
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path

BRANCH = "ci/v8-nightly"
MODEL = "openai/gpt-5.6-sol"
ROOT = ARTIFACTS = TEST_LOG = BUILD_LOG = BEP = REPORT = None
KEY = KEY_FILE = None


def init(root):
    global ROOT, ARTIFACTS, TEST_LOG, BUILD_LOG, BEP, REPORT
    ROOT = Path(root)
    ARTIFACTS = ROOT / "artifacts"
    TEST_LOG = ARTIFACTS / "v8-nightly-test.log"
    BUILD_LOG = ARTIFACTS / "v8-nightly-build.log"
    BEP = ARTIFACTS / "v8-nightly.bep.json"
    REPORT = ROOT / "ci/v8-nightly-report.md"


def run(args, *, check=True, capture=False, cwd=None, env=None, timeout=None):
    print("+", shlex.join(map(str, args)), flush=True)
    return subprocess.run(
        list(map(str, args)),
        cwd=cwd or ROOT,
        check=check,
        text=True,
        env=env,
        timeout=timeout,
        stdout=subprocess.PIPE if capture else None,
    )


def output(args, **kwargs):
    return run(args, capture=True, **kwargs).stdout.strip()


def logged(args, path):
    print("+", shlex.join(map(str, args)), flush=True)
    with Path(path).open("a") as log:
        return subprocess.run(
            list(map(str, args)), cwd=ROOT, stdout=log, stderr=subprocess.STDOUT
        ).returncode


def clean_artifacts():
    ARTIFACTS.mkdir(exist_ok=True)
    for path in (TEST_LOG, BUILD_LOG, BEP, REPORT):
        path.unlink(missing_ok=True)


def setup_git(project):
    global KEY
    KEY = os.environ.pop("SSH_PRIVKEY")
    _install_key()

    for name, value in (
        ("user.name", "svc_ew_v8_patches"),
        ("user.email", "ew-v8-patches@cloudflare.com"),
        ("safe.directory", ROOT),
    ):
        run(["git", "config", "--global", "--add", name, value])
    remote = f"ssh://git@gitlab.cfdata.org/cloudflare/ew/{project}.git"
    run(["git", "remote", "set-url", "origin", remote])


def _install_key():
    global KEY_FILE
    KEY_FILE = tempfile.NamedTemporaryFile("w")
    KEY_FILE.write(KEY + "\n")
    KEY_FILE.flush()
    command = f"ssh -i {shlex.quote(KEY_FILE.name)} -o IdentitiesOnly=yes"
    os.environ["GIT_SSH_COMMAND"] = command


@contextmanager
def revoked():
    """Remove the SSH key from disk for the duration.

    The AI agent runs with unrestricted permissions, so the private key must
    not be reachable while it is working. The key is reinstalled once the agent
    returns.
    """
    KEY_FILE.close()  # NamedTemporaryFile removes the file on close
    os.environ.pop("GIT_SSH_COMMAND", None)
    try:
        yield
    finally:
        _install_key()


def commit(message, pathspecs=(".",)):
    run(["git", "add", "-A", "--", *pathspecs])
    if run(["git", "diff", "--cached", "--quiet"], check=False).returncode:
        run(["git", "commit", "-q", "-m", message])

    ref = f"refs/heads/{BRANCH}"

    # hackily work around cfsetup read-only HTTP token rewrite
    env = os.environ | {
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
    }

    run(
        ["git", "fetch", "--no-tags", "origin", f"+{ref}:refs/remotes/origin/{BRANCH}"],
        check=False,
        env=env,
    )
    if run(
        ["git", "push", "--force-with-lease", "-o", "ci.skip", "origin", f"HEAD:{ref}"],
        check=False,
        env=env,
    ).returncode:
        raise RuntimeError(f"Failed to push {BRANCH} to origin")


def _opencode_config():
    model = MODEL.rsplit("/", 1)[-1]
    options = {
        "baseURL": "https://gateway.ai.cloudflare.com/v1/27b146402af2103944379f33841b6234/project-gateway/openai",
        "apiKey": os.environ["OPENCODE_AI_GATEWAY_API_TOKEN"],
    }
    return {
        "model": MODEL,
        "provider": {
            "openai": {"options": options, "models": {model: {"name": model}}}
        },
        "permission": {"*": "allow"},
    }


def ai(repo, mode, old, new, detail=None):
    docs = "docs/v8-updates.md" if repo == "workerd" else "docs/v8.md"
    prompt = (
        f"Read AGENTS.md and {docs} first. Keep changes specific to the V8 update "
        f"from {old} to {new}. Do not edit CI automation, credential handling, or "
        "Git submodules directly. Do not commit or push. Write the outcome and "
        "verification to ci/v8-nightly-report.md. "
    )
    if repo == "edgeworker":
        prompt += (
            "For any focused Bazel command, pass `--config=ci-common "
            "--config=cloudflare-direct-access --remote_executor= --remote_cache=`. "
        )
    if mode == "rebase":
        finish = (
            f"python3 ci/v8_update.py finish {new}"
            if repo == "workerd"
            else "./v8.sh finish-update"
        )
        # workerd AGENTS.md says to not merge V8 patches without human review, so try to prompt that advice out
        prompt += (
            f"Update {repo} to the new V8 version. This unattended probe is explicitly "
            "authorized to resolve V8 conflicts without human confirmation. Preserve "
            "each patch's intent and stop if a resolution is unclear. Resolve the rebase "
            f"in {detail}, continue it, then run `{finish}`. Do not run builds, tests, "
            "or other Bazel commands; the orchestrator will run them with the correct "
            "CI configuration."
        )
    elif mode == "build":
        prompt += (
            "The V8 update and patch rebase are already complete. Diagnose "
            f"{BUILD_LOG} and make only durable tracked source changes. Do not modify "
            "generated Bazel outputs or V8 build artifacts. If verification is blocked, "
            "report that explicitly and do not claim it passed."
        )
    elif repo == "workerd":
        prompt += (
            "The V8 update and patch rebase are already complete. Diagnose "
            f"{TEST_LOG} and {BEP}, and make only durable tracked source changes. Do "
            "not modify generated Bazel outputs or V8 build artifacts. The orchestrator "
            "will rerun all tests. If verification is blocked, report that explicitly "
            "and do not claim it passed."
        )
    else:
        prompt += (
            "The V8 update and patch rebase are already complete. Diagnose "
            f"{TEST_LOG} and {BEP}, and make only durable tracked source changes. Do "
            "not modify generated Bazel outputs or V8 build artifacts. Pass the local "
            f"V8 override at {detail} to focused tests. If verification is blocked, "
            "report that explicitly and do not claim it passed."
        )

    config_dir = Path.home() / ".config/opencode"
    config_dir.mkdir(parents=True, exist_ok=True)
    config = config_dir / "opencode.json"
    config.write_text(json.dumps(_opencode_config()))
    try:
        with revoked():
            result = run(
                ["opencode", "run", prompt],
                check=False,
                timeout=int(os.environ.get("OPENCODE_TIMEOUT", "7200")),
            )
    except subprocess.TimeoutExpired:
        return False

    if REPORT.is_file():
        print("=== AI report ===\n" + REPORT.read_text())

    return result.returncode == 0
