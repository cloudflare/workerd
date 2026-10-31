# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Facts the generator takes from the checked-in TypeScript declarations
(`types/generated-snapshot/index.d.ts`), which the RTTI does not carry:

- Parameter names. The RTTI's `argNames` are empty unless the build supplies the external
  parameter-name table (`src/workerd/tools/param-extractor.rs` output), so names come from a
  declaration with the same member name and parameter count. Names never affect the Wasm
  signature; without a match the generator uses `arg0`, `arg1`, ...
- Deprecation. A member is deprecated if each of its declarations, or the declaration of its
  class, carries a `@deprecated` JSDoc tag. See "Deprecated members" in `../conventions.md`.

Only the subset of TypeScript that the generated declarations use is understood: class,
interface and `declare var X: {...}` blocks with one member per line, and `declare function`.
"""

import re
from pathlib import Path
from typing import NamedTuple

# The repository root is four levels above this directory.
SNAPSHOT = (
    Path(__file__).absolute().parents[4] / "types" / "generated-snapshot" / "index.d.ts"
)
SOURCE = "types/generated-snapshot/index.d.ts"

DECL = re.compile(
    r"^[ \t]*(?:export\s+)?(?:declare\s+)?"
    r"(?:(?:abstract\s+)?(?:class|interface)\s+(\w+)|var\s+(\w+):\s*\{)",
    re.MULTILINE,
)
# A member line: a method, constructor (`constructor(` or `new (`), accessor or property.
MEMBER = re.compile(
    r"^\s*(?:(?:public|protected|private|static|abstract|readonly|async)\s+)*"
    r"(?:(get|set)\s+)?(\w+)\??\s*(?:<[^()]*>)?\s*"
    r"(?:\((.*)\))?\s*[:;]"
)
FUNCTION = re.compile(
    r"^declare function (\w+)\s*(?:<[^()]*>)?\s*\((.*)\)\s*:", re.MULTILINE
)
DEPRECATED_TAG = re.compile(r"@deprecated\b[ \t]*([^\n]*)")


class Declaration(NamedTuple):
    """One declaration of a member: its parameter names (None for an accessor or property, or
    if they could not be parsed), and the text of its `@deprecated` tag (None if it has none)."""

    params: list | None
    deprecated: str | None


def _split_params(text):
    text = text.replace("=>", "  ")
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    names = []
    for p in out:
        m = re.match(r"\s*(?:\.\.\.)?(\w+)\s*\??\s*:", p)
        if not m:
            return None
        names.append(m.group(1))
    return names


def _block(text, start):
    """The text of the `{...}` block that starts at or after `start`."""
    i = text.index("{", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i + 1 : j]
    return ""


def _join_parens(text):
    """Joins declarations whose parameter lists span several lines onto one line. Parentheses
    in comments do not count."""
    out, depth, i = [], 0, 0
    while i < len(text):
        if text.startswith(("/*", "//"), i):
            end = text.find("*/" if text[i + 1] == "*" else "\n", i)
            end = len(text) if end < 0 else end + (2 if text[i + 1] == "*" else 0)
            out.append(text[i:end])
            i = end
            continue
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        out.append(" " if ch == "\n" and depth > 0 else ch)
        i += 1
    return "".join(out)


def _deprecated(doc):
    """The text of the `@deprecated` tag in a JSDoc comment, or None if it has none."""
    if doc is None:
        return None
    m = DEPRECATED_TAG.search(doc)
    if m is None:
        return None
    return m.group(1).replace("*/", "").strip()


def _doc_before(text, pos):
    """The JSDoc comment that ends right before `pos` (only whitespace in between), or None."""
    head = text[:pos].rstrip()
    if not head.endswith("*/"):
        return None
    start = head.rfind("/*")
    return head[start:] if head.startswith("/**", start) else None


def _members(body):
    """Yields (accessor, name, parameter text, JSDoc) for each member line of a block body."""
    depth, doc, comment = 0, None, None
    for line in body.splitlines():
        stripped = line.strip()
        if comment is not None:
            comment.append(stripped)
            if "*/" in stripped:
                doc = "\n".join(comment) if comment[0].startswith("/**") else None
                comment = None
            continue
        if stripped.startswith("/*"):
            if "*/" in stripped:
                doc = stripped if stripped.startswith("/**") and depth == 0 else None
            else:
                comment = [stripped] if depth == 0 else ["/*"]
            continue
        if depth == 0 and stripped:
            m = MEMBER.match(line)
            if m:
                yield m.group(1), m.group(2), m.group(3), doc
            doc = None
        depth += line.count("{") - line.count("}")


class Declarations:
    """The members of the classes, interfaces and global functions that the declarations
    declare, keyed by (lower-cased class name or "globalthis", member name)."""

    def __init__(self, text):
        self.members = {}
        text = _join_parens(text)
        for m in DECL.finditer(text):
            cls = (m.group(1) or m.group(2)).lower()
            class_deprecated = _deprecated(_doc_before(text, m.start()))
            for accessor, name, params, doc in _members(_block(text, m.start())):
                member = "constructor" if name == "new" else name
                names = None
                if accessor is None and params is not None:
                    names = _split_params(params)
                deprecated = _deprecated(doc)
                if deprecated is None:
                    deprecated = class_deprecated
                self.members.setdefault((cls, member), []).append(
                    Declaration(names, deprecated)
                )
        for m in FUNCTION.finditer(text):
            self.members.setdefault(("globalthis", m.group(1)), []).append(
                Declaration(
                    _split_params(m.group(2)), _deprecated(_doc_before(text, m.start()))
                )
            )

    @classmethod
    def load(cls, path=SNAPSHOT):
        path = Path(path)
        return cls(path.read_text() if path.exists() else "")

    def param_names(self, cls, member, count):
        """Parameter names of a declaration of `cls.member` with `count` parameters, or None."""
        for d in self.members.get((cls.lower(), member), []):
            if d.params is not None and len(d.params) == count:
                return d.params
        return None

    def deprecation(self, cls, member):
        """Why `cls.member` is deprecated, if every declaration of it is; otherwise None."""
        decls = self.members.get((cls.lower(), member), [])
        if not decls or any(d.deprecated is None for d in decls):
            return None
        texts = sorted({d.deprecated for d in decls if d.deprecated})
        return "@deprecated in {}{}".format(
            SOURCE, ": " + " ".join(texts) if texts else ""
        )
