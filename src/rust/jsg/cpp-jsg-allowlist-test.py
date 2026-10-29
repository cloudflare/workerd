# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Checks that Rust JSG's C++ shims don't add new uses of C++ JSG.

C++ JSG will be removed, so Rust JSG must not depend on it (src/rust/jsg/README.md, "Safety
rules", rule 3). This test counts the C++ JSG names below in each shim file, ignoring comments,
and compares the counts with the allowlist of existing uses. A count above the allowlist fails.
A count below it also fails, so that the allowlist is lowered when a use is removed.

Usage: cpp-jsg-allowlist-test.py ALLOWLIST FILE...

The allowlist has one entry per line, `<path> <construct> <count>`, with `#` comments. A line
`<path> interop` puts the file in the interop tier instead (src/rust/jsg-cpp, the binding between
Rust JSG and C++ JSG resources): it may use the constructs in INTEROP_CONSTRUCTS without limit,
and is checked against the allowlist for the rest like any other file. The path
is the file's path from the workerd repository root. When workerd is an external repository of
another Bazel workspace, `$(location)` adds a prefix such as `external/<repo>/` to every path, so
the test removes the prefix that comes before the allowlist's own path.
"""

import re
import sys
from pathlib import Path

# Allowlist name -> pattern. `jsg::Wrappable`, `jsg::GcVisitor` and the module registries are not
# listed, because Rust JSG still shares them with C++ JSG.
CONSTRUCTS = {
    "jsg::Lock": r"\bjsg::Lock\b",
    "JSG_TRY": r"\bJSG_(?:TRY|CATCH)\b",
    "tryCatch": r"(?:\.|->)tryCatch\s*\(",
    "jsg::check": r"\bjsg::check\b",
    "JsExceptionThrown": r"\bJsExceptionThrown\b",
    "createTunneledException": r"\bcreateTunneledException\b",
    "makeInternalError": r"\bmakeInternalError\b",
    "jsg::Data": r"\bjsg::(?:Data|Value|JsValue|JsRef|V8Ref|JsObject|JsString)\b",
    "jsg::Ref": r"\bjsg::(?:Ref|Promise|Function|Optional|Sequence|Object)\b",
    "JSG_REQUIRE": r"\bJSG_(?:REQUIRE|FAIL_REQUIRE|ASSERT|FAIL_ASSERT|WARN_ONCE)\b",
    "liftKj": r"\bliftKj\b",
    "jsg::throw": r"\bjsg::throw[A-Z]\w*",
    # The interop crate, which only the interop tier may use.
    "jsg-cpp": r"\bjsg[-_]cpp\b",
}

# What the interop tier may use: enough to hold, wrap and unwrap C++ JSG resources through the
# isolate's TypeHandlers and to turn a C++ exception into a pending JavaScript one. Not allowed,
# even there: JSG_TRY/tryCatch, createTunneledException, makeInternalError, jsg::check, liftKj and
# JSG_REQUIRE, for which the interop has its own V8-level replacements (callCpp).
INTEROP_CONSTRUCTS = {
    "jsg::Lock",
    "JsExceptionThrown",
    "jsg::Data",
    "jsg::Ref",
    "jsg::throw",
    "jsg-cpp",
}

ALTERNATIVES = """\
Use V8 directly instead:
- Instead of JSG_TRY/tryCatch/createTunneledException: catch with a v8::TryCatch and return the
  exception through a MaybeLocal out-parameter.
- Instead of jsg::check/JsExceptionThrown: return a v8::MaybeLocal or v8::Maybe and let Rust turn
  an empty result into a jsg::Error.
- Instead of jsg::Lock/makeInternalError: create the error with V8 and call
  isolate->ThrowException.
- Instead of jsg::Data/jsg::Value: hold the handle in Rust as a jsg::v8::Global or TracedGlobal."""

ALLOWLIST_PATH = "src/rust/jsg/cpp-jsg-allowlist.txt"

# Comments and string and character literals, matched left to right so that `//` inside a string
# or `/*` inside a line comment is read correctly. Literals are kept, so a name inside a string
# still counts.
TOKEN = re.compile(
    r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.DOTALL
)


def strip_comments(source):
    return TOKEN.sub(lambda m: "" if m.group().startswith("/") else m.group(), source)


def count_constructs(path):
    source = strip_comments(path.read_text(encoding="utf-8"))
    return {
        name: len(re.findall(pattern, source))
        for name, pattern in CONSTRUCTS.items()
        if re.search(pattern, source)
    }


def read_allowlist(path):
    allowed = {}
    interop = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if fields[1:] == ["interop"]:
            interop.add(fields[0])
            continue
        if len(fields) != 3 or not fields[2].isdigit() or fields[1] not in CONSTRUCTS:
            sys.exit(
                f"{path}:{number}: expected `<path> <construct> <count>`, got: {line}"
            )
        allowed[(fields[0], fields[1])] = int(fields[2])
    for file, construct in allowed:
        if file in interop and construct in INTEROP_CONSTRUCTS:
            sys.exit(
                f"{path}: `{file}` is in the interop tier, which may use {construct} without"
                " a count. Remove its line."
            )
    return allowed, interop


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    allowlist = Path(argv[0])
    allowed, interop = read_allowlist(allowlist)
    prefix = allowlist.as_posix().removesuffix(ALLOWLIST_PATH)
    files = {Path(arg).as_posix().removeprefix(prefix): Path(arg) for arg in argv[1:]}

    problems = [
        f"{allowlist}: `{file}` is in the interop tier but not checked (moved or deleted?)."
        " Remove its `interop` line, or add the file to CPP_SHIMS in src/rust/jsg/BUILD.bazel."
        for file in sorted(interop - files.keys())
    ]
    for key, count in sorted(allowed.items()):
        file, construct = key
        if file not in files:
            problems.append(
                f"{allowlist}: `{file}` is not checked (moved or deleted?). Remove its"
                f" `{construct}` line, or add the file to CPP_SHIMS in src/rust/jsg/BUILD.bazel."
            )
        elif count == 0:
            problems.append(
                f"{allowlist}: `{file} {construct} 0` allows nothing. Remove the line."
            )

    for file, path in sorted(files.items()):
        found = count_constructs(path)
        for construct in CONSTRUCTS:
            if file in interop and construct in INTEROP_CONSTRUCTS:
                continue
            actual = found.get(construct, 0)
            expected = allowed.get((file, construct), 0)
            if actual > expected:
                problems.append(
                    f"{file}: {actual} use(s) of {construct}, {expected} allowed. Don't add new"
                    " uses of C++ JSG to Rust JSG; see src/rust/jsg/README.md,"
                    " Safety rules, rule 3."
                )
            elif actual < expected:
                problems.append(
                    f"{file}: {actual} use(s) of {construct}, {expected} allowed. Lower the"
                    f" count in the allowlist to `{file} {construct} {actual}`"
                    + (" (or delete the line)" if actual == 0 else "")
                    + "."
                )

    if problems:
        print("\n".join(problems))
        print()
        print(ALTERNATIVES)
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])
