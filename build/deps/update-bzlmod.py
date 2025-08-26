#!/usr/bin/env uv run
# /// script
# requires-python = ">=3.13"
# dependencies = [
#     "libcst"
# ]
# ///

import argparse
import json
import subprocess
import types
import urllib.request
from pathlib import Path

import libcst as cst
import libcst.matchers as m

ResolvedVersion = str | None
DepsDict = dict[str, ResolvedVersion]


class DepVersionsCollector(m.MatcherDecoratableTransformer):
    def __init__(self):
        self.deps: DepsDict = {}
        self.current_name: str | None = None
        super().__init__()

    @m.call_if_inside(m.Call(func=m.Name("bazel_dep")))
    @m.visit(m.Arg(keyword=m.Name("name")))
    def find_bazel_deps(self, node: cst.Arg):
        self.current_name = node.value.raw_value

    @m.call_if_inside(m.Call(func=m.Name("bazel_dep")))
    @m.visit(m.Arg(keyword=m.Name("version")))
    def find_version(self, node: cst.Arg):
        if not self.current_name:
            return

        self.deps[self.current_name] = node.value.raw_value


class DepUpdateTransform(m.MatcherDecoratableTransformer):
    def __init__(self, deps: DepsDict):
        self.deps: DepsDict = deps
        self.current_name: str | None = None
        super().__init__()

    @m.call_if_inside(m.Call(func=m.Name("bazel_dep")))
    @m.visit(m.Arg(keyword=m.Name("name")))
    def find_bazel_deps(self, node: cst.Arg):
        self.current_name = node.value.raw_value

    @m.call_if_inside(m.Call(func=m.Name("bazel_dep")))
    @m.leave(m.Arg(keyword=m.Name("version")))
    def update_version(self, orig: cst.Arg, mod: cst.Arg) -> cst.Arg:
        if not self.current_name:
            return mod

        new_version = self.deps.get(self.current_name)
        if not new_version:
            return mod

        return mod.with_changes(value=cst.SimpleString(repr(new_version)))


def get_bcr_version(name: str) -> ResolvedVersion:
    module_versions_url = f"https://bcr.bazel.build/modules/{name}/metadata.json"
    with urllib.request.urlopen(module_versions_url) as res:
        try:
            meta = json.load(res, object_hook=types.SimpleNamespace)
            # FIXME: Is the last version listed always latest?
            new_version = meta.versions[-1]
            print(f"{name} = {new_version}")
        except Exception as exc:
            print(exc)
            return None
        else:
            return new_version


def main():
    cmd = argparse.ArgumentParser()
    cmd.add_argument(
        "module_bazel_path", type=Path, nargs="?", default=Path("MODULE.bazel")
    )
    args = cmd.parse_args()

    module_bazel = cst.parse_module(args.module_bazel_path.read_text())
    collector = DepVersionsCollector()
    module_bazel.visit(collector)
    updated_deps: DepsDict = {
        name: get_bcr_version(name) or version
        for name, version in collector.deps.items()
    }
    new_module_bazel = module_bazel.visit(DepUpdateTransform(updated_deps))

    args.module_bazel_path.write_text(new_module_bazel.code)

    # Trigger a reformat because libcst slightly mangles formatting
    subprocess.run(["./tools/cross/format.py", "git"])


if __name__ == "__main__":
    main()
