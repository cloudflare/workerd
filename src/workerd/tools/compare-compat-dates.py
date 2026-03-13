#!/usr/bin/env python3
# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""
Compares compatibility flags between PR and baseline to validate date requirements.
Used by CI to ensure new flags have dates sufficiently far in the future.
"""

import argparse
import json
import os
import sys
from datetime import datetime, timedelta
from pathlib import Path


def load_flags(filepath):
    """Load and parse JSON flags file."""
    try:
        with Path(filepath).open() as f:
            data = json.load(f)
            return data.get("flags", [])
    except FileNotFoundError:
        print(f"Error: File not found: {filepath}", file=sys.stderr)
        sys.exit(2)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {filepath}: {e}", file=sys.stderr)
        sys.exit(2)


def find_new_flags(pr_flags, baseline_flags):
    """Find flags that exist in PR but not in baseline."""
    baseline_names = {flag["enableFlag"] for flag in baseline_flags}
    return [flag for flag in pr_flags if flag["enableFlag"] not in baseline_names]


def calculate_min_date(min_days):
    """Calculate minimum allowed date (today + min_days)."""
    return (datetime.now() + timedelta(days=min_days)).strftime("%Y-%m-%d")


def find_violations(new_flags, min_date):
    """Find new flags with dates earlier than min_date."""
    violations = []
    for flag in new_flags:
        date = flag.get("date", "")
        if date and date < min_date:
            violations.append(
                {
                    "field": flag["field"],
                    "enableFlag": flag["enableFlag"],
                    "date": date,
                    "minDate": min_date,
                }
            )
    return violations


def generate_violation_report(violations, min_days):
    """Generate markdown report for violations."""
    report_lines = [
        "## ⚠️ Compatibility Date Validation Failed",
        "",
        f"New compatibility flags must have dates at least **{min_days} days** in the future.",
        "",
        "| Field | Flag Name | Current Date | Minimum Required |",
        "|-------|-----------|--------------|------------------|",
    ]

    report_lines.extend(
        [
            f"| `{v['field']}` | `{v['enableFlag']}` | {v['date']} | {v['minDate']} |"
            for v in violations
        ]
    )

    report_lines.extend(
        [
            "",
            "### How to fix",
            f"Update the `$compatEnableDate` in `compatibility-date.capnp` to a date >= **{violations[0]['minDate']}**.",
            "",
            "### Bypass",
            "If this is urgent, add the `urgent-compat-flag` label to bypass this check.",
        ]
    )

    return "\n".join(report_lines)


def set_github_output(key, value):
    """Set GitHub Actions output variable."""
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with Path(github_output).open("a") as f:
            f.write(f"{key}={value}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Compare compatibility flags between PR and baseline"
    )
    parser.add_argument("pr_flags", help="Path to PR flags JSON file")
    parser.add_argument("baseline_flags", help="Path to baseline flags JSON file")
    parser.add_argument(
        "--min-days",
        type=int,
        default=7,
        help="Minimum days in the future for new flag dates (default: 7)",
    )

    args = parser.parse_args()

    # Load flags from both files
    pr_flags = load_flags(args.pr_flags)
    baseline_flags = load_flags(args.baseline_flags)

    # Find new flags
    new_flags = find_new_flags(pr_flags, baseline_flags)

    if not new_flags:
        print("No new compatibility flags found.")
        set_github_output("has_violations", "false")
        return 0

    # Calculate minimum date
    min_date = calculate_min_date(args.min_days)
    print(f"Minimum allowed date: {min_date} (today + {args.min_days} days)")

    # Print new flags
    print("\nNew flags found:")
    for flag in new_flags:
        print(f"  - {flag['enableFlag']}")

    # Check for violations
    violations = find_violations(new_flags, min_date)

    if not violations:
        print(f"\n✓ All new compatibility flag dates are valid (>= {min_date})")
        set_github_output("has_violations", "false")
        return 0

    # Report violations
    set_github_output("has_violations", "true")

    # Generate and write markdown report
    report = generate_violation_report(violations, args.min_days)
    with Path("violation-report.md").open("w") as f:
        f.write(report)

    # Print report to stdout
    print(f"\n{report}")

    # Print GitHub error
    print(
        f"\n::error::New compatibility flags must have dates at least {args.min_days} days in the future"
    )

    return 1


if __name__ == "__main__":
    sys.exit(main())
