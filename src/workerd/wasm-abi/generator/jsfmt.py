# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Renders data (dicts, lists, strings, numbers, booleans, None) as JavaScript or JSON source text
in the layout Prettier produces with the repository's settings (`.prettierrc.json`), so that the
generated files need no formatting pass and `prettier --check` accepts them.

The rules are the subset of Prettier's that this data needs: a list or dict is printed on one
line if it fits in 80 columns, and otherwise with one element per line; a list of two or more
dicts (or lists) that each have two or more elements always takes one element per line. In
JavaScript, strings take single quotes, keys that are identifiers are unquoted, and multi-line
lists and dicts have trailing commas.
"""

import json
import re

WIDTH = 80
INDENT = 2
IDENTIFIER = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*\Z")


def _string(s, js):
    if not js:
        return json.dumps(s, ensure_ascii=False)
    quote = '"' if s.count("'") > s.count('"') else "'"
    body = json.dumps(s, ensure_ascii=False)[1:-1].replace('\\"', '"')
    return quote + body.replace(quote, "\\" + quote) + quote


def _key(k, js):
    return k if js and IDENTIFIER.match(k) else _string(k, js)


def _scalar(v, js):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return _string(v, js)
    raise TypeError(f"cannot render {type(v).__name__}")


def _flat(v, js):
    if isinstance(v, dict):
        if not v:
            return "{}"
        body = ", ".join(f"{_key(k, js)}: {_flat(x, js)}" for k, x in v.items())
        return "{ " + body + " }"
    if isinstance(v, list):
        return "[" + ", ".join(_flat(x, js) for x in v) + "]"
    return _scalar(v, js)


def _must_break(v):
    """Prettier always breaks a list of two or more dicts, or of two or more lists, that each
    have more than one element."""
    if not isinstance(v, list) or len(v) < 2:
        return False
    return all(isinstance(x, dict) and len(x) > 1 for x in v) or all(
        isinstance(x, list) and len(x) > 1 for x in v
    )


def _render(v, indent, used, tail, js):
    """Renders `v`, which starts at column `used` of a line indented by `indent` and is followed
    by `tail` more characters on its last line."""
    flat = _flat(v, js)
    if not isinstance(v, (dict, list)) or not v:
        return flat
    if not _must_break(v) and used + len(flat) + tail <= WIDTH:
        return flat
    inner = indent + INDENT
    items = list(v.items()) if isinstance(v, dict) else [(None, x) for x in v]
    lines = []
    for i, (k, x) in enumerate(items):
        comma = "," if js or i < len(items) - 1 else ""
        prefix = "" if k is None else _key(k, js) + ": "
        body = _render(x, inner, inner + len(prefix), len(comma), js)
        lines.append(" " * inner + prefix + body + comma)
    opening, closing = ("{", "}") if isinstance(v, dict) else ("[", "]")
    return opening + "\n" + "\n".join(lines) + "\n" + " " * indent + closing


def json_text(v):
    """A JSON file holding `v`."""
    return _render(v, 0, 0, 0, False) + "\n"


def js_default_export(v):
    """The statement `export default <v>;`."""
    head = "export default "
    return head + _render(v, 0, len(head), 1, True) + ";\n"
