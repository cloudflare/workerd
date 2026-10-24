# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Checks workerd-unsafe-continuation-capture against the annotations in its fixture.

Each annotation (a run of `//` comment lines) covers the lines after it, up to the next comment or
the closing `}` of the function (a line that is exactly `}` or `  }`):
- `expected:` / `expected diagnostic`: at least one diagnostic. If the annotation names identifiers
  in backticks, every diagnostic in the region must name one of them.
- `expected (unless ...)`: as `expected` by default, none with the check options set.
- `expected (when ...)`: none by default, as `expected` with the check options set.
- starts with `// safe`: no diagnostic.
Every diagnostic must lie in an `expected` region. The fixture runs once without and once with
OPTIONS, the CheckOptions its `(when ...)` and `(unless ...)` annotations refer to.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
CLANG_TIDY = ROOT / "tools/clang_tidy"
PLUGIN = ROOT / "tools/clang-tidy/libworkerd-lint.so"
FIXTURE = ROOT / "tools/clang-tidy/tests/unsafe-continuation-capture-test.c++"
CHECK = "workerd-unsafe-continuation-capture"
OPTIONS = (
    "{CheckOptions: ["
    f"{{key: {CHECK}.AsyncSinks, value: 'myproject::mySink'}}, "
    f"{{key: {CHECK}.OwningCaptureTypes, value: 'myproject::MyOwn'}}, "
    f"{{key: {CHECK}.ResultOwningSinks, value: 'myproject::Db::runWithConn'}}]}}"
)
DIAGNOSTIC = re.compile(
    r".*unsafe-continuation-capture-test\.c\+\+:(\d+):\d+: (?:error|warning): "
    r"unsafe .* capture of (\S+) in lambda"
)


def run_check(config):
    args = [CLANG_TIDY, f"--load={PLUGIN}", f"--checks=-*,{CHECK}"]
    if config:
        args.append(f"--config={config}")
    result = subprocess.run(
        [*args, FIXTURE, "--", "-std=c++23", "-Wno-undefined-internal"],
        capture_output=True,
        text=True,
        check=False,
    )
    output = result.stdout + result.stderr
    diagnostics = [
        (int(m.group(1)), m.group(2))
        for m in map(DIAGNOSTIC.match, output.splitlines())
        if m
    ]
    # The fixture always produces diagnostics, so none means the plugin or the fixture failed.
    if not diagnostics:
        sys.exit(f"clang-tidy reported no {CHECK} diagnostics:\n{output}")
    return diagnostics


def annotations(lines, with_options):
    i = next(k for k, line in enumerate(lines) if line and not line.startswith("//"))
    while i < len(lines):
        if not lines[i].strip().startswith("//"):
            i += 1
            continue
        j = i
        while j < len(lines) and lines[j].strip().startswith("//"):
            j += 1
        text = " ".join(line.strip()[2:].strip() for line in lines[i:j])
        low = text.lower()
        kind = None
        if "expected (unless" in low:
            kind = "safe" if with_options else "expected"
        elif "expected (when" in low:
            kind = "expected" if with_options else "safe"
        elif re.search(r"expected(:| diagnostic)", low):
            kind = "expected"
        elif lines[i].strip().lower().startswith("// safe"):
            kind = "safe"
        if kind:
            end = j
            while (
                end < len(lines)
                and lines[end] not in ("}", "  }")
                and not lines[end].strip().startswith("//")
            ):
                end += 1
            names = {n.lstrip("&") for n in re.findall(r"`(&?\w+)`", text)}
            # Lines are 1-based: the region is lines j + 1 through end.
            yield i + 1, j + 1, end, kind, names
        i = j


def verify(lines, with_options):
    diagnostics = run_check(OPTIONS if with_options else None)
    failures = []
    claimed = set()
    for start, first, end, kind, names in annotations(lines, with_options):
        inside = [d for d in diagnostics if first <= d[0] <= end]
        claimed.update(inside)
        if kind == "safe" and inside:
            failures.append(f"line {start}: marked safe but flagged: {inside}")
        elif kind == "expected" and not inside:
            failures.append(f"line {start}: expected a diagnostic, got none")
        elif kind == "expected" and names and any(d[1] not in names for d in inside):
            failures.append(
                f"line {start}: flagged {[d[1] for d in inside]}, expected {names}"
            )
    failures += [
        f"diagnostic outside any expected region: {d}"
        for d in diagnostics
        if d not in claimed
    ]
    return failures


def main():
    lines = FIXTURE.read_text().split("\n")
    failures = [
        f"{'with' if with_options else 'without'} options: {failure}"
        for with_options in (False, True)
        for failure in verify(lines, with_options)
    ]
    if failures:
        sys.exit("\n".join(failures))


if __name__ == "__main__":
    main()
