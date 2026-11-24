"""Tests for the Chromium Rust crate alignment in v8_update.py.

Cargo, Bazel and the Chromium lockfile are mocked, so the tests check which
versions the helper asks Cargo for, and when it fails.
"""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import v8_update

V8_DEPS = (
    "'third_party/rust': Var('chromium_url') + '/chromium/src/third_party/rust.git' + '@' + "
    f"'{'0' * 40}',"
)

CARGO_TOML = """\
[package]
name = "direct-cargo-bazel-deps"

[dependencies]
icu_calendar = { version = "=2.3.0", default-features = false }
temporal_rs = { version = "=0.2.6", default-features = false, features = ["float64_representable_durations"] }
tinystr = "0.8"
writeable = "=0.6.2"
"""

# The versions of each crate that CHROMIUM_CRATE_ROOTS reach in Chromium's
# Cargo.lock. They reach two versions of yoke.
CHROMIUM = {
    "icu_calendar": ["2.2.1"],
    "temporal_rs": ["0.2.6"],
    "tinystr": ["0.8.2"],
    "writeable": ["0.6.2"],
    "yoke": ["0.7.5", "0.8.1"],
    "zerovec": ["0.11.5"],
}


class UpdateChromiumCratesTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        cargo_toml = Path(directory.name) / "Cargo.toml"
        cargo_toml.write_text(CARGO_TOML)
        for target, value in (
            ("CARGO_TOML", cargo_toml),
            ("_chromium_crate_versions", mock.Mock(return_value=CHROMIUM)),
        ):
            patcher = mock.patch.object(v8_update, target, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.cargo = self._patch("_cargo")
        self.run_command = self._patch("run")
        self.tree = self._patch("_chromium_crate_tree")

    def _patch(self, name):
        patcher = mock.patch.object(v8_update, name)
        self.addCleanup(patcher.stop)
        return patcher.start()

    def test_aligned_tree_changes_nothing(self):
        self.tree.return_value = {
            ("icu_calendar", "2.2.1"),
            ("temporal_rs", "0.2.6"),
            ("zerovec", "0.11.5"),
        }
        v8_update._update_chromium_crates(V8_DEPS)
        self.cargo.assert_not_called()
        self._assert_repinned_bazel_lockfile()

    def test_moves_exact_pins_with_cargo_add(self):
        aligned = {("icu_calendar", "2.2.1"), ("temporal_rs", "0.2.6")}
        self.tree.side_effect = [
            {("icu_calendar", "2.3.0"), ("temporal_rs", "0.2.6")},
            aligned,
            aligned,
        ]
        v8_update._update_chromium_crates(V8_DEPS)
        self.cargo.assert_called_once_with("add", "icu_calendar@=2.2.1")
        self._assert_repinned_bazel_lockfile()

    def test_moves_unpinned_crates_with_cargo_update(self):
        self.tree.side_effect = [
            {("tinystr", "0.8.3"), ("zerovec", "0.11.6")},
            {("tinystr", "0.8.3"), ("zerovec", "0.11.6")},
            {("tinystr", "0.8.2"), ("zerovec", "0.11.5")},
        ]
        v8_update._update_chromium_crates(V8_DEPS)
        self.assertCountEqual(
            self.cargo.call_args_list,
            [
                mock.call("update", "tinystr@0.8.3", "--precise", "0.8.2", check=False),
                mock.call(
                    "update", "zerovec@0.11.6", "--precise", "0.11.5", check=False
                ),
            ],
        )
        self._assert_repinned_bazel_lockfile()

    def test_fails_when_cargo_refuses_a_version(self):
        # Cargo refuses to downgrade zerovec, Chromium doesn't reach unicode-ident,
        # and Chromium reaches two versions of yoke.
        tree = {("zerovec", "0.11.6"), ("unicode-ident", "1.0.0"), ("yoke", "0.8.1")}
        self.tree.side_effect = [tree, tree, tree]
        with self.assertRaisesRegex(
            ValueError,
            r"unicode-ident 1\.0\.0 \(Chromium: none\), "
            r"yoke 0\.8\.1 \(Chromium: 0\.7\.5, 0\.8\.1\), "
            r"zerovec 0\.11\.6 \(Chromium: 0\.11\.5\)",
        ):
            v8_update._update_chromium_crates(V8_DEPS)
        self.cargo.assert_called_once_with(
            "update", "zerovec@0.11.6", "--precise", "0.11.5", check=False
        )
        self.run_command.assert_not_called()

    def _assert_repinned_bazel_lockfile(self):
        self.run_command.assert_called_once_with(
            [
                "bazel",
                "fetch",
                "--repo_env=CARGO_BAZEL_REPIN=1",
                "--repo=@crates_vendor",
            ]
        )


def _package(name, version, *dependencies):
    return {"name": name, "version": version, "dependencies": list(dependencies)}


class ReachableVersionsTest(unittest.TestCase):
    def _reachable(self, *packages):
        with mock.patch.object(v8_update, "CHROMIUM_CRATE_ROOTS", ("root",)):
            return v8_update._reachable_versions({"package": list(packages)})

    def test_follows_the_versions_the_root_depends_on(self):
        # The lock holds two versions of b and c. The root reaches b 0.2.0, which
        # reaches c 1.0.0, so neither b 0.1.0 nor c 2.0.0 is reached.
        chromium = self._reachable(
            _package("root", "1.0.0", "a", "b 0.2.0"),
            _package("a", "1.0.0"),
            _package("b", "0.1.0", "c 2.0.0"),
            _package(
                "b",
                "0.2.0",
                "c 1.0.0 (registry+https://github.com/rust-lang/crates.io-index)",
            ),
            _package("c", "1.0.0"),
            _package("c", "2.0.0"),
            _package("unreached", "1.0.0"),
        )
        self.assertEqual(
            chromium,
            {"root": ["1.0.0"], "a": ["1.0.0"], "b": ["0.2.0"], "c": ["1.0.0"]},
        )
        self.assertEqual(v8_update._chromium_version(chromium, "b"), "0.2.0")
        self.assertIsNone(v8_update._chromium_version(chromium, "unreached"))

    def test_crate_reached_at_two_versions_has_no_chromium_version(self):
        chromium = self._reachable(
            _package("root", "1.0.0", "a", "b 0.1.0"),
            _package("a", "1.0.0", "b 0.2.0"),
            _package("b", "0.1.0"),
            _package("b", "0.2.0"),
        )
        self.assertEqual(chromium["b"], ["0.1.0", "0.2.0"])
        self.assertIsNone(v8_update._chromium_version(chromium, "b"))

    def test_fails_when_the_root_is_locked_at_two_versions(self):
        with self.assertRaisesRegex(
            ValueError, r"exactly one version of root, but locks 1\.0\.0, 2\.0\.0"
        ):
            self._reachable(_package("root", "1.0.0"), _package("root", "2.0.0"))


if __name__ == "__main__":
    unittest.main()
