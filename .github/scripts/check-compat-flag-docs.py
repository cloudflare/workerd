#!/usr/bin/env python3
"""
Verify that every new compatibility flag with a default-on date
(via $compatEnableDate or $impliedByAfterDate) has matching documentation
in cloudflare/cloudflare-docs, either already merged to the production
branch or in an open PR with at least one approving review.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

CAPNP_PATH = "src/workerd/io/compatibility-date.capnp"
DOCS_REPO = "cloudflare/cloudflare-docs"
DOCS_BRANCH = "production"
DOCS_DIR = "src/content/compatibility-flags"


# ---------------------------------------------------------------------------
# capnp parsing
# ---------------------------------------------------------------------------


def parse_compat_flags(content: str) -> dict:
    """Return {enable_flag_name: info} for every flag that carries a
    $compatEnableDate or $impliedByAfterDate annotation."""

    result = {}

    # Split on field boundaries.  Each field starts with:
    #     fieldName @N :Type
    blocks = re.split(r"\n(?=\s*\w+\s+@\d+\s*)", content)

    for block in blocks:
        m = re.match(r"\s*(\w+)\s+@(\d+)\s*:\s*(\w+)", block)
        if not m:
            continue

        field_name = m.group(1)

        # Skip obsolete fields.
        if field_name.startswith("obsolete"):
            continue

        flag_m = re.search(r'\$compatEnableFlag\s*\(\s*"([^"]+)"', block)
        if not flag_m:
            continue
        enable_flag = flag_m.group(1)

        date_m = re.search(r'\$compatEnableDate\s*\(\s*"([^"]+)"', block)
        implied_m = re.search(r"\$impliedByAfterDate", block)
        is_experimental = "$experimental" in block

        if date_m or implied_m:
            result[enable_flag] = {
                "field_name": field_name,
                "enable_date": date_m.group(1) if date_m else None,
                "has_implied_by": implied_m is not None,
                "experimental": is_experimental,
            }

    return result


# ---------------------------------------------------------------------------
# git helpers
# ---------------------------------------------------------------------------


def get_base_content(base_sha: str) -> str:
    """Return the capnp file at *base_sha*, or '' if it doesn't exist."""
    r = subprocess.run(
        ["git", "show", f"{base_sha}:{CAPNP_PATH}"],
        capture_output=True,
        text=True,
    )
    return r.stdout if r.returncode == 0 else ""


def resolve_base_sha() -> str:
    """Determine the base SHA to diff against."""
    # 1. Explicit env var (set by the workflow for pull_request events).
    sha = os.environ.get("GITHUB_BASE_SHA")
    if sha:
        return sha

    # 2. Pull from the GitHub event payload.
    event_path = os.environ.get("GITHUB_EVENT_PATH")
    if event_path and Path(event_path).is_file():
        with Path(event_path).open() as f:
            event = json.load(f)
        sha = event.get("pull_request", {}).get("base", {}).get("sha") or event.get(
            "merge_group", {}
        ).get("base_sha")
        if sha:
            return sha

    # 3. Fallback - merge-base with origin/main.
    r = subprocess.run(
        ["git", "merge-base", "HEAD", "origin/main"],
        capture_output=True,
        text=True,
    )
    if r.returncode == 0:
        return r.stdout.strip()

    print("::error::Could not determine base SHA to diff against.")
    sys.exit(1)


# ---------------------------------------------------------------------------
# cloudflare-docs checks (via `gh` CLI)
# ---------------------------------------------------------------------------


def gh(*args, **kwargs) -> subprocess.CompletedProcess:
    """Run a `gh` command and return the CompletedProcess."""
    return subprocess.run(
        ["gh", *args],
        capture_output=True,
        text=True,
        **kwargs,
    )


def doc_exists_on_production(slug: str) -> bool:
    """Return True if *slug*.md exists on the production branch."""
    r = gh(
        "api",
        f"repos/{DOCS_REPO}/contents/{DOCS_DIR}/{slug}.md?ref={DOCS_BRANCH}",
        "-q",
        ".name",
    )
    return r.returncode == 0


def search_open_docs_prs(flag_name: str) -> list[dict]:
    """Search for open PRs in cloudflare-docs mentioning *flag_name*.

    Returns a (possibly empty) list of ``{"number": int, "title": str}``.
    """
    r = gh(
        "search",
        "prs",
        "--repo",
        DOCS_REPO,
        "--state",
        "open",
        flag_name,
        "--json",
        "number,title",
        "--limit",
        "10",
    )
    if r.returncode != 0 or not r.stdout.strip():
        return []
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return []


def pr_has_approval(pr_number: int) -> bool:
    """Return True if *pr_number* in cloudflare-docs has ≥1 approving review."""
    r = gh(
        "api",
        f"repos/{DOCS_REPO}/pulls/{pr_number}/reviews",
        "--jq",
        '[.[] | select(.state == "APPROVED")] | length',
    )
    if r.returncode != 0:
        return False
    try:
        return int(r.stdout.strip()) > 0
    except (ValueError, TypeError):
        return False


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main() -> None:
    base_sha = resolve_base_sha()

    with Path(CAPNP_PATH).open() as f:
        head_content = f.read()

    base_content = get_base_content(base_sha)

    head_flags = parse_compat_flags(head_content)
    base_flags = parse_compat_flags(base_content)

    # Flags that are *new* or that *gained* a default-on date in this PR.
    new_flags: dict[str, dict] = {}
    for name, info in head_flags.items():
        if name not in base_flags:
            new_flags[name] = info
        else:
            old = base_flags[name]
            if info.get("enable_date") and not old.get("enable_date"):
                new_flags[name] = info
            elif info.get("has_implied_by") and not old.get("has_implied_by"):
                new_flags[name] = info

    if not new_flags:
        print("No new compatibility flags with default-on dates detected.")
        return

    print(f"Found {len(new_flags)} new flag(s) with default-on dates:\n")
    for name, info in sorted(new_flags.items()):
        date = info.get("enable_date") or "(implied-by-after-date)"
        print(f"  {name}  {date}")
    print()

    # ------------------------------------------------------------------
    # Check each flag for documentation.
    # ------------------------------------------------------------------
    undocumented: list[str] = []
    needs_approval: list[tuple[str, list[dict]]] = []

    for flag_name in sorted(new_flags):
        slug = flag_name.replace("_", "-")

        # 1. Already on the production branch?
        if doc_exists_on_production(slug):
            print(f"  ok   {flag_name}  (on production)")
            continue

        # 2. Open PR that mentions the flag?
        prs = search_open_docs_prs(flag_name)
        if not prs:
            # Also try the slug form (hyphens).
            prs = search_open_docs_prs(slug)

        if not prs:
            undocumented.append(flag_name)
            print(f"  FAIL {flag_name}  (no docs found)")
            continue

        # 3. Does any of those PRs have an approving review?
        approved = False
        for pr in prs:
            if pr_has_approval(pr["number"]):
                print(f"  ok   {flag_name}  (approved PR #{pr['number']})")
                approved = True
                break

        if not approved:
            needs_approval.append((flag_name, prs))
            nums = ", ".join(f"#{p['number']}" for p in prs)
            print(f"  WAIT {flag_name}  (PR {nums} needs approval)")

    # ------------------------------------------------------------------
    # Emit GitHub Actions error annotations.
    # ------------------------------------------------------------------
    print()
    errors: list[str] = []

    for flag_name in undocumented:
        slug = flag_name.replace("_", "-")
        msg = (
            f"Compatibility flag `{flag_name}` adds a default-on date "
            f"but has no documentation in {DOCS_REPO}.  "
            f"Please open a PR there adding "
            f"`{DOCS_DIR}/{slug}.md` "
            f"and get at least one approving review."
        )
        errors.append(msg)
        print(f"::error file={CAPNP_PATH}::{msg}")

    for flag_name, prs in needs_approval:
        nums = ", ".join(f"#{p['number']}" for p in prs)
        msg = (
            f"Compatibility flag `{flag_name}` has a docs PR ({nums}) "
            f"in {DOCS_REPO} but it still needs at least one approving review."
        )
        errors.append(msg)
        print(f"::error file={CAPNP_PATH}::{msg}")

    if errors:
        print()
        print(f"{len(errors)} flag(s) need documentation before this PR can merge.")
        print(
            f"\nSee https://github.com/{DOCS_REPO}/tree/{DOCS_BRANCH}/{DOCS_DIR} "
            "for examples."
        )
        sys.exit(1)

    print("All new flags with default-on dates are documented.")


if __name__ == "__main__":
    main()
