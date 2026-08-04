"""Loader and writer for JSONC, the dialect of JSON with `//` comments that our
`*.jsonc` config files use.

Only `//` line comments are supported, anywhere whitespace is allowed. Block
comments and trailing commas are not part of the dialect and are reported as
syntax errors, with positions referring to the original text.

Comments are kept in a tree that mirrors the shape of the data rather than in
the data itself, so `Document.data` is exactly what `json.load()` would have
returned:

    doc = jsonc.loads(path.read_text())
    doc.data["repositories"][0]["freeze_version"] = "1.2.3"
    doc.comments.members["repositories"].members[0].before.append("// pinned")
    path.write_text(jsonc.dumps(doc) + "\\n")

Array elements are keyed by position, so inserting or removing one leaves the
comments of its successors on the wrong elements; move them yourself when doing
that. Comments on members that are not in the data are ignored when writing.
"""

import io
import json
import unittest
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, TextIO


@dataclass
class Comments:
    """The comments on one value, and on the values within it.

    Comments include their `//`. `before` holds the whole lines above the value,
    `inline` is the remainder of the value's own line, `end` holds the lines
    within an array or object past its last member, and `members` holds the
    comments on each element or member, by index or key.

    The root's `end` reaches to the end of the document, so a comment trailing
    the root value is written back inside its closing bracket. A root that is
    neither an array nor an object has no `end` to write, and drops them.
    """

    before: list[str] = field(default_factory=list)
    inline: str | None = None
    end: list[str] = field(default_factory=list)
    members: dict[Any, "Comments"] = field(default_factory=dict)


@dataclass
class Document:
    """Parsed JSONC: plain Python data and the comments around it."""

    data: Any
    comments: Comments = field(default_factory=Comments)


def loads(text: str) -> Document:
    """Parse JSONC text."""
    # Comments are blanked out rather than deleted so that every offset, and so
    # every position json reports in a syntax error, still refers to `text`.
    clean, trivia = _blank_comments(text)
    data = json.loads(clean)
    reader = _Reader(clean, trivia)
    reader.space()
    header = reader.pending(None)  # taken first: comments are consumed in order
    comments = reader.value()
    comments.before = header
    # The root value is not really the end of a JSONC document: comments can
    # trail it. They have no value of their own to sit above, so they join the
    # end of the root, moving inside its brackets when written back.
    reader.i = len(clean)
    comments.end += reader.pending(comments)
    return Document(data, comments)


def load(fp: TextIO) -> Document:
    """Parse JSONC read from a file."""
    return loads(fp.read())


def dumps(
    obj: Document | Any, *, indent: int | str = 2, ensure_ascii: bool = False
) -> str:
    """Serialize a Document, or plain data, as JSONC without a trailing newline.

    Unlike `json.dumps()` this emits non-ASCII text literally, to avoid
    rewriting strings that a human put in the file.
    """
    if indent is None:
        raise ValueError("comments need indented lines to live on")
    doc = obj if isinstance(obj, Document) else Document(obj)
    pad = indent if isinstance(indent, str) else " " * indent
    out: list[str] = []

    def scalar(value: Any) -> str:
        return json.dumps(value, ensure_ascii=ensure_ascii)

    def comments(lines: list[str], depth: int) -> None:
        out.extend(f"{pad * depth}{line}\n" for line in lines)

    def name(key: Any) -> str:
        # json spells a key that is not a string as the string it prints as.
        return scalar(key if isinstance(key, str) else json.dumps(key)) + ": "

    def write(data: Any, node: Comments, depth: int) -> None:
        """Write `data`, whose own line is already indented."""
        if isinstance(data, dict):
            items = [(name(k), k, v) for k, v in data.items()]
            brackets = "{}"
        elif isinstance(data, (list, tuple)):
            items = [("", i, v) for i, v in enumerate(data)]
            brackets = "[]"
        else:
            out.append(scalar(data))
            return

        if not items and not node.end:
            out.append(brackets)
            return
        out.append(brackets[0] + "\n")
        for position, (key, step, value) in enumerate(items):
            child = node.members.get(step, Comments())
            comments(child.before, depth + 1)
            out.append(pad * (depth + 1) + key)
            write(value, child, depth + 1)
            out.append("" if position == len(items) - 1 else ",")
            out.append("" if child.inline is None else " " + child.inline)
            out.append("\n")
        comments(node.end, depth + 1)
        out.append(pad * depth + brackets[1])

    comments(doc.comments.before, 0)
    write(doc.data, doc.comments, 0)
    if doc.comments.inline is not None:
        out.append(" " + doc.comments.inline)
    return "".join(out)


def dump(obj: Document | Any, fp: TextIO, **kwargs) -> None:
    """Write a Document, or plain data, to a file as JSONC, ending the line."""
    fp.write(dumps(obj, **kwargs) + "\n")


@dataclass
class _Trivium:
    """A comment, where it was found, and whether anything else shares its
    line, which decides whether it belongs to a value or sits above one."""

    offset: int
    text: str
    inline: bool


def _blank_comments(text: str) -> tuple[str, list[_Trivium]]:
    """Replace every comment with spaces of its own width, returning the
    comments. Blank lines are not recorded, so writing a file back drops them."""
    out: list[str] = []
    trivia: list[_Trivium] = []
    i, bare = 0, True
    while i < len(text):
        char = text[i]
        if char == '"':
            end = _past_string(text, i)
            out.append(text[i:end])
            bare, i = False, end
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = len(text) if end < 0 else end
            trivia.append(_Trivium(i, text[i:end].rstrip(), not bare))
            out.append(" " * (end - i))
            i = end
        elif char == "\n":
            out.append(char)
            bare, i = True, i + 1
        else:
            bare = bare and char.isspace()
            out.append(char)
            i += 1
    return "".join(out), trivia


def _past_string(text: str, i: int) -> int:
    """Index just past the string literal starting at `i`, or the end of an
    unterminated one, which json will complain about later."""
    i += 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
        elif text[i] == '"':
            return i + 1
        else:
            i += 1
    return len(text)


def _past_space(text: str, i: int) -> int:
    while i < len(text) and text[i].isspace():
        i += 1
    return i


class _Reader:
    """Walks JSON text that comments have been blanked out of, alongside those
    comments, to build a Comments tree mirroring the data. The text is known to
    parse, having already been through `json.loads()`."""

    def __init__(self, text: str, trivia: list[_Trivium]):
        self.text = text
        self.trivia = trivia
        self.i = 0  # position in the text
        self.t = 0  # comments consumed so far

    def space(self) -> None:
        self.i = _past_space(self.text, self.i)

    def pending(self, previous: Comments | None) -> list[str]:
        """The comments before the position reached, which sit above whatever
        comes next -- except that one sharing a line with the value just read is
        that value's `inline` comment instead."""
        start = self.t
        while self.t < len(self.trivia) and self.trivia[self.t].offset < self.i:
            self.t += 1
        taken = self.trivia[start : self.t]
        if taken and taken[0].inline and previous is not None:
            previous.inline = taken[0].text
            taken = taken[1:]
        return [trivium.text for trivium in taken]

    def value(self) -> Comments:
        """Read the value at the position reached, which is not whitespace."""
        node = Comments()
        char = self.text[self.i]
        if char not in "{[":
            self.scalar()
            return node

        closing = "}" if char == "{" else "]"
        self.i += 1
        previous = None
        while True:
            self.space()
            if self.text[self.i] == closing:
                node.end = self.pending(previous)
                self.i += 1
                return node
            before = self.pending(previous)
            step = self.key() if closing == "}" else len(node.members)
            previous = node.members[step] = self.value()
            previous.before = before
            self.space()
            if self.text[self.i] == ",":
                self.i += 1

    def key(self) -> str:
        """Read a member's name, leaving the position at its value."""
        end = _past_string(self.text, self.i)
        name = json.loads(self.text[self.i : end])
        self.i = _past_space(self.text, _past_space(self.text, end) + 1)  # past ':'
        return name

    def scalar(self) -> None:
        """Skip a string, number, boolean or null."""
        if self.text[self.i] == '"':
            self.i = _past_string(self.text, self.i)
            return
        while self.i < len(self.text) and not (
            self.text[self.i] in ",]}" or self.text[self.i].isspace()
        ):
            self.i += 1


class RoundTripTest(unittest.TestCase):
    def assertRoundTrip(self, text: str) -> Comments:
        doc = loads(text)
        clean, _ = _blank_comments(text)
        self.assertEqual(doc.data, json.loads(clean))
        self.assertEqual(dumps(doc) + "\n", text)
        return doc.comments

    def test_our_config_files(self):
        for path in sorted(Path(__file__).parent.glob("*.jsonc")):
            with self.subTest(file=path.name):
                doc = loads(path.read_text())
                self.assertTrue(doc.comments.members)
                # Not byte for byte: these are formatted by prettier, which
                # keeps short arrays on one line, while json always breaks them.
                reread = loads(dumps(doc))
                self.assertEqual(reread.data, doc.data)
                self.assertEqual(reread.comments, doc.comments)

    def test_comment_positions(self):
        comments = self.assertRoundTrip(
            """\
// header
{
  // above a member
  "a": 1, // beside a member
  "b": [
    // above an element
    2,
    3 // beside the last element
    // dangling in an array
  ],
  "c": {}
  // dangling in an object
}
"""
        )
        members = comments.members
        self.assertEqual(comments.before, ["// header"])
        self.assertEqual(members["a"].before, ["// above a member"])
        self.assertEqual(members["a"].inline, "// beside a member")
        self.assertEqual(members["b"].members[0].before, ["// above an element"])
        self.assertEqual(members["b"].members[1].inline, "// beside the last element")
        self.assertEqual(members["b"].end, ["// dangling in an array"])
        self.assertEqual(comments.end, ["// dangling in an object"])

    def test_comments_trailing_the_document_move_inside(self):
        doc = loads('{\n  "a": 1\n} // beside\n// trailing\n')
        self.assertEqual(doc.comments.inline, "// beside")
        self.assertEqual(doc.comments.end, ["// trailing"])
        moved = '{\n  "a": 1\n  // trailing\n} // beside\n'
        self.assertEqual(dumps(doc) + "\n", moved)
        self.assertRoundTrip(moved)  # and it stays put from then on

    def test_comment_in_empty_container(self):
        comments = self.assertRoundTrip('{\n  "a": {\n    // nothing yet\n  }\n}\n')
        self.assertEqual(comments.members["a"].end, ["// nothing yet"])

    def test_blank_lines_are_dropped(self):
        doc = loads('{\n  "a": 1,\n\n  // b\n  "b": 2\n}\n')
        self.assertEqual(doc.comments.members["b"].before, ["// b"])
        self.assertEqual(dumps(doc) + "\n", '{\n  "a": 1,\n  // b\n  "b": 2\n}\n')

    def test_awkward_positions(self):
        # A comment where a value was expected belongs to the member it is in.
        self.assertEqual(
            self.assertRoundTrip('{\n  "a": 1 // c\n}\n').members["a"].inline, "// c"
        )
        self.assertEqual(loads('{"a": // c\n 1}').comments.members["a"].inline, "// c")
        # One beside an opening bracket has to move to the line below it.
        self.assertEqual(
            loads('{ // c\n"a": 1}').comments.members["a"].before, ["// c"]
        )

    def test_unusual_keys(self):
        comments = self.assertRoundTrip(
            '{\n  // c\n  "a/b~c": 1,\n  "": 2,\n  "\u00e9": 3\n}\n'
        )
        self.assertEqual(comments.members["a/b~c"].before, ["// c"])
        self.assertEqual(list(comments.members), ["a/b~c", "", "\u00e9"])
        # An escaped key names the same member as the character it stands for.
        self.assertEqual(list(loads('{"\\u00e9": 3}').comments.members), ["\u00e9"])

    def test_comments_are_not_found_in_strings(self):
        comments = self.assertRoundTrip(
            '{\n  "a": "// not a comment \\" //",\n  "b": 1\n}\n'
        )
        self.assertEqual(comments.members["a"], Comments())

    def test_scalar_and_empty_documents(self):
        self.assertRoundTrip("// lonely\n42\n")
        self.assertRoundTrip("{}\n")
        self.assertRoundTrip('[\n  [],\n  {},\n  "x"\n]\n')
        self.assertEqual(loads("[]").data, [])


class WriterTest(unittest.TestCase):
    def test_plain_data_matches_json(self):
        data = {"a": [1, 2.5, None, True], "b": {}, "c": [[]], "d": -1.5e10, 1: "k"}
        self.assertEqual(dumps(data, ensure_ascii=True), json.dumps(data, indent=2))
        self.assertEqual(dumps(data, indent="\t"), json.dumps(data, indent="\t"))

    def test_added_comments(self):
        doc = Document({"repositories": [{"name": "v8"}]})
        entry = Comments(before=["// keep in sync"])
        entry.members["name"] = Comments(inline="// the engine")
        doc.comments.before.append("// generated")
        doc.comments.members["repositories"] = Comments(members={0: entry})
        self.assertEqual(
            dumps(doc),
            """\
// generated
{
  "repositories": [
    // keep in sync
    {
      "name": "v8" // the engine
    }
  ]
}""",
        )

    def test_non_ascii_is_literal(self):
        self.assertEqual(dumps({"\u00e9": "\u2013"}), '{\n  "\u00e9": "\u2013"\n}')

    def test_comments_on_absent_members_are_ignored(self):
        doc = Document({"a": 1}, Comments(members={"b": Comments(before=["// gone"])}))
        self.assertEqual(dumps(doc), '{\n  "a": 1\n}')

    def test_dump_appends_newline(self):
        fp = io.StringIO()
        dump({"a": 1}, fp)
        self.assertEqual(fp.getvalue(), '{\n  "a": 1\n}\n')


class SyntaxErrorTest(unittest.TestCase):
    def test_position_ignores_comments(self):
        with self.assertRaises(json.JSONDecodeError) as caught:
            loads('{\n  // a comment\n  "a": oops\n}\n')
        self.assertEqual((caught.exception.lineno, caught.exception.colno), (3, 8))

    def test_block_comments_are_rejected(self):
        self.assertRaises(json.JSONDecodeError, loads, "/* nope */ {}")

    def test_trailing_commas_are_rejected(self):
        self.assertRaises(json.JSONDecodeError, loads, '{"a": 1,}')


if __name__ == "__main__":
    unittest.main()
