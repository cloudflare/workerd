# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Unit tests of the generator's rules for include lists, released and deprecated imports, of the TypeScript
declaration reader and of the Prettier-compatible renderer, on synthetic inputs."""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).absolute().parent))

import generate
import jsfmt
import typescript

STR = {"kind": "string", "name": "kj::String"}
INT = {"kind": "number", "name": "int"}
VOID = {"kind": "voidt"}


def method(name, args, ret):
    return {
        "method": {
            "name": name,
            "returnType": ret,
            "args": args,
            "static": False,
            "argNames": [],
        }
    }


def structure(name, fqn, members):
    return {
        "name": name,
        "fullyQualifiedName": fqn,
        "members": members,
        "extends": None,
        "tsName": "",
    }


def rtti(*members):
    thing = structure("Thing", "workerd::api::Thing", list(members))
    scope = structure(
        "ServiceWorkerGlobalScope",
        generate.GLOBAL_SCOPE,
        [{"nested": {"structure": thing, "name": "Thing"}}],
    )
    return {"groups": [{"name": "test", "structures": [thing, scope]}]}


A = method("a", [STR], VOID)
B = method("b", [INT], STR)


def build(members, ts="", record=None, complete=True, include=("Thing#a", "Thing#b")):
    spec = generate.ModuleSpec("cf:test/v1", [("workerd::api::Thing", include)], [])
    lowerer = generate.Lowerer(rtti(*members), typescript.Declarations(ts))
    return generate.build_module(spec, lowerer, record, None, complete)


def names(module):
    return [i["name"] for i in module.imports]


def record_of(module, *keep):
    return {"imports": [i for i in module.imports if i["name"] in keep]}


class IncludeLists(unittest.TestCase):
    def test_members_the_list_does_not_name_are_not_added(self):
        module = build([A, B], include=["Thing#b"])
        self.assertEqual(names(module), ["Thing#b"])
        self.assertEqual(module.excluded, [])

    def test_imports_follow_the_include_list(self):
        self.assertEqual(
            names(build([A, B], include=["Thing#b", "Thing#a"])), ["Thing#b", "Thing#a"]
        )

    def test_an_included_member_must_exist(self):
        with self.assertRaisesRegex(
            generate.GenerationError,
            "include list names Thing#a, which is not a member",
        ):
            build([B])

    def test_an_older_compat_date_may_lack_included_members(self):
        self.assertEqual(names(build([B], complete=False)), ["Thing#b"])


class ReleasedImports(unittest.TestCase):
    def test_new_imports_are_appended_after_released_ones(self):
        record = record_of(build([B], include=["Thing#b"]), "Thing#b")
        self.assertEqual(names(build([A, B], record=record)), ["Thing#b", "Thing#a"])

    def test_a_released_import_cannot_leave_the_include_list(self):
        record = record_of(build([A, B]), "Thing#a", "Thing#b")
        with self.assertRaisesRegex(
            generate.GenerationError,
            "released import Thing#b is no longer in the include list. .* can't be removed",
        ):
            build([A, B], record=record, include=["Thing#a"])

    def test_a_released_import_cannot_lose_its_member(self):
        record = record_of(build([A, B]), "Thing#a", "Thing#b")
        with self.assertRaisesRegex(
            generate.GenerationError, "released import Thing#b .* can't be removed"
        ):
            build([A], record=record)

    def test_an_older_compat_date_may_lack_released_imports(self):
        record = record_of(build([A, B]), "Thing#a", "Thing#b")
        self.assertEqual(names(build([A], record=record, complete=False)), ["Thing#a"])

    def test_a_released_import_cannot_change_its_abi(self):
        record = record_of(build([B], include=["Thing#b"]), "Thing#b")
        with self.assertRaisesRegex(
            generate.GenerationError, "Thing#b changed .* can't change"
        ):
            build([method("b", [INT, INT], STR)], record=record, include=["Thing#b"])

    def test_a_released_import_may_change_what_only_the_api_sees(self):
        record = record_of(build([B], include=["Thing#b"]), "Thing#b")
        usv = {"kind": "string", "name": "jsg::USVString"}
        module = build([method("b", [INT], usv)], record=record, include=["Thing#b"])
        self.assertEqual(module.imports[0]["resultType"], "string(jsg::USVString)")


class DeprecatedMembers(unittest.TestCase):
    TS = "declare class Thing {\n  a(text: string): void;\n  /** @deprecated Use a(). */\n  b(n: number): string;\n}\n"

    def test_an_included_deprecated_member_cannot_be_added(self):
        with self.assertRaisesRegex(
            generate.GenerationError,
            "Thing#b is deprecated .* remove it from the include list .* list it in "
            "DEPRECATED",
        ):
            build([A, B], ts=self.TS)

    def test_an_included_listed_member_cannot_be_added(self):
        with (
            mock.patch.dict(generate.DEPRECATED, {"Thing#b": "Use a()."}),
            self.assertRaisesRegex(
                generate.GenerationError,
                r"Thing#b is deprecated \(Use a\(\).\) .* remove it from the include list",
            ) as raised,
        ):
            build([A, B])
        self.assertNotIn("list it in DEPRECATED", str(raised.exception))

    def test_a_listed_member_is_recorded_as_excluded(self):
        with mock.patch.dict(generate.DEPRECATED, {"Thing#b": "Use a()."}):
            module = build([A, B], ts=self.TS, include=["Thing#a"])
        self.assertEqual(names(module), ["Thing#a"])
        self.assertEqual(module.excluded, [{"name": "Thing#b", "reason": "Use a()."}])

    def test_a_released_member_that_becomes_deprecated_stays_marked(self):
        record = record_of(build([A, B]), "Thing#a", "Thing#b")
        module = build([A, B], ts=self.TS, record=record)
        self.assertEqual(names(module), ["Thing#a", "Thing#b"])
        self.assertEqual(
            module.imports[1]["deprecated"],
            "@deprecated in types/generated-snapshot/index.d.ts: Use a().",
        )
        self.assertNotIn("deprecated", module.imports[0])

    def test_a_listed_released_member_stays_marked(self):
        record = record_of(build([A, B]), "Thing#a", "Thing#b")
        with mock.patch.dict(generate.DEPRECATED, {"Thing#a": "Superseded."}):
            module = build([A, B], record=record)
        self.assertEqual(module.imports[0]["deprecated"], "Superseded.")
        self.assertEqual(module.excluded, [])


class TypeScriptDeclarations(unittest.TestCase):
    TS = """\
declare class Thing {
  /**
   * Gets it.
   * @deprecated Use other().
   */
  get value(): string;
  set value(v: string);
  /** @deprecated Old form. */
  call(a: string): void;
  call(a: string, b: number): void;
  /** Joins (an unbalanced parenthesis in a comment. */
  join(
    first: string,
    second?: string,
  ): string;
}
/** @deprecated The whole class. */
interface Old {
  run(): void;
}
/** @deprecated Use fetch(). */
declare function get(url: string): Promise<void>;
"""

    def test_deprecation(self):
        d = typescript.Declarations(self.TS)
        self.assertIsNone(
            d.deprecation("Thing", "value")
        )  # The setter is not deprecated.
        self.assertIsNone(d.deprecation("Thing", "call"))  # Only one overload is.
        self.assertIsNone(d.deprecation("Thing", "join"))
        self.assertIn("The whole class.", d.deprecation("Old", "run"))
        self.assertIn("Use fetch().", d.deprecation("globalThis", "get"))

    def test_parameter_names(self):
        d = typescript.Declarations(self.TS)
        self.assertEqual(d.param_names("Thing", "call", 2), ["a", "b"])
        self.assertEqual(d.param_names("Thing", "join", 2), ["first", "second"])
        self.assertIsNone(d.param_names("Thing", "call", 3))


class PrettierLayout(unittest.TestCase):
    def test_short_values_stay_on_one_line(self):
        self.assertEqual(
            jsfmt.js_default_export({"a": [1, "x"], "b": {"c": None}}),
            "export default { a: [1, 'x'], b: { c: null } };\n",
        )

    def test_long_values_break_one_element_per_line(self):
        v = {"list": ["y" * 40, "z" * 40], "short": ["y", "z"]}
        self.assertEqual(
            jsfmt.json_text(v),
            f'{{\n  "list": [\n    "{"y" * 40}",\n    "{"z" * 40}"\n  ],\n'
            '  "short": ["y", "z"]\n}\n',
        )

    def test_lists_of_several_multi_key_dicts_always_break(self):
        self.assertEqual(
            jsfmt.js_default_export([{"a": 1, "b": 2}, {"a": 3, "b": 4}]),
            "export default [\n  { a: 1, b: 2 },\n  { a: 3, b: 4 },\n];\n",
        )
        self.assertEqual(
            jsfmt.js_default_export([{"a": 1}, {"a": 3}]),
            "export default [{ a: 1 }, { a: 3 }];\n",
        )

    def test_strings_take_single_quotes_unless_they_contain_more(self):
        self.assertEqual(
            jsfmt.js_default_export(["it's", 'a "b"']),
            'export default ["it\'s", \'a "b"\'];\n',
        )


if __name__ == "__main__":
    unittest.main()
