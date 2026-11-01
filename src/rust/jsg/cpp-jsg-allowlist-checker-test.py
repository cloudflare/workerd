# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Tests cpp-jsg-allowlist-test.py on small allowlists and shim files."""

import contextlib
import importlib.util
import io
import os
import tempfile
import unittest
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "checker", Path(__file__).with_name("cpp-jsg-allowlist-test.py")
)
checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(checker)

INTEROP_FILE = "src/rust/jsg-cpp/jsg-cpp.h"


class CheckerTest(unittest.TestCase):
    def check(self, allowlist, files):
        """Runs the checker; returns its output, or None if it passed."""
        with tempfile.TemporaryDirectory() as root:
            allowlist_path = Path(root, checker.ALLOWLIST_PATH)
            allowlist_path.parent.mkdir(parents=True)
            allowlist_path.write_text(allowlist)
            paths = []
            for name, source in files.items():
                path = Path(root, name)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
                paths.append(os.fspath(path))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                try:
                    checker.main([os.fspath(allowlist_path), *paths])
                except SystemExit as exit:
                    return f"{output.getvalue()}{exit.code or ''}"
            return None

    def check_interop(self, source):
        return self.check(f"{INTEROP_FILE} interop\n", {INTEROP_FILE: source})

    def test_interop_tier_allows_its_constructs(self):
        self.assertIsNone(
            self.check_interop(
                "jsg::Lock& js; jsg::Ref<T> r; jsg::Optional<int> o; jsg::Object* p;"
                " jsg::V8Ref<v8::Object> w; throw jsg::JsExceptionThrown();"
                " jsg::throwInternalError(isolate, e); namespace jsg_cpp {}"
            )
        )

    def test_interop_tier_rejects_other_constructs(self):
        for source, construct in [
            ("jsg::throwTunneledException(isolate, e);", "jsg::throw"),
            ("jsg::throwTypeError(isolate, e);", "jsg::throw"),
            ("jsg::Promise<int> p;", "jsg::Promise"),
            ("jsg::Function<void()> f;", "jsg::Promise"),
            ("jsg::Sequence<int> s;", "jsg::Promise"),
            ("jsg::JsValue v;", "jsg::Data"),
            ("jsg::JsRef<jsg::JsValue> v;", "jsg::Data"),
            ("jsg::Value v;", "jsg::Data"),
            (
                "auto e = createTunneledException(isolate, x);",
                "createTunneledException",
            ),
        ]:
            with self.subTest(source):
                output = self.check_interop(source)
                self.assertIsNotNone(output)
                self.assertIn(f"use(s) of {construct}, 0 allowed", output)

    def test_interop_tier_is_only_for_jsg_cpp(self):
        output = self.check(
            "src/rust/jsg/ffi.c++ interop\n", {"src/rust/jsg/ffi.c++": "jsg::Ref<T> r;"}
        )
        self.assertIsNotNone(output)
        self.assertIn(
            "only files in src/rust/jsg-cpp/ may be in the interop tier", output
        )

    def test_other_shims_count_interop_constructs(self):
        output = self.check("", {"src/rust/jsg/ffi.c++": "jsg::V8Ref<v8::Object> w;"})
        self.assertIsNotNone(output)
        self.assertIn("use(s) of jsg::V8Ref, 0 allowed", output)


if __name__ == "__main__":
    unittest.main()
