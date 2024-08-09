#!/usr/bin/env python3

import logging
import os
import re
import subprocess
from argparse import ArgumentParser, Namespace
from typing import List, Optional

CLANG_FORMAT = os.environ.get("CLANG_FORMAT", "clang-format")


def parse_args() -> Namespace:
    parser = ArgumentParser()
    parser.add_argument(
        "--check",
        help="only check for files requiring formatting; don't actually format them",
        action="store_true",
        default=False,
    )
    subparsers = parser.add_subparsers(dest="subcommand")
    git_parser = subparsers.add_parser(
        "git", help="Apply format to changes tracked by git"
    )
    git_parser.add_argument(
        "--source",
        help="consider files modified in the specified commit-ish; if not specified, defaults to all changes in the working directory",
        type=str,
        required=False,
        default=None,
    )
    git_parser.add_argument(
        "--target",
        help="consider files modified since the specified commit-ish; defaults to HEAD",
        type=str,
        required=False,
        default="HEAD",
    )
    git_parser.add_argument(
        "--staged",
        help="consider files with staged modifications only",
        action="store_true",
        default=False,
    )
    options = parser.parse_args()
    if options.subcommand == "git":
        if options.staged:
            if options.source is not None or options.target != "HEAD":
                logging.error(
                    "--staged cannot be used with --source or --target; use --staged with --source=HEAD"
                )
                exit(1)
    return options


def check_clang_format() -> bool:
    try:
        # Run clang-format with --version to check its version
        output = subprocess.check_output([CLANG_FORMAT, "--version"]).decode("utf-8")
        major, _, _ = re.search(r"version\s*(\d+)\.(\d+)\.(\d+)", output).groups()
        if int(major) < 18:
            logging.error("clang-format version must be at least 18.0.0")
            exit(1)
    except FileNotFoundError:
        # Clang-format is not in the PATH
        logging.error("clang-format not found in the PATH")
        exit(1)


def find_cpp_files(dir_path) -> List[str]:
    files = []
    for root, _, filenames in os.walk(dir_path):
        for filename in filenames:
            if filename.endswith((".c++", ".h")):
                files.append(os.path.join(root, filename))
    return files


def clang_format(files: List[str], check=False):
    if not files:
        logging.info("No changes to format")
        exit(0)
    if check:
        result = subprocess.run(
            [CLANG_FORMAT, "--verbose", "--dry-run", "--Werror"] + files, check=False
        )
        if result.returncode != 0:
            logging.error("Code has lint. Fix with: python ./tools/cross/format.py")
            exit(1)
    else:
        subprocess.run([CLANG_FORMAT, "--verbose", "-i"] + files, check=False)


def git_get_modified_files(
    target: str, source: Optional[str], staged: bool
) -> List[str]:
    if staged:
        files_in_diff = (
            subprocess.check_output(
                ["git", "diff", "--diff-filter=d", "--name-only", "--cached"]
            )
            .decode("utf-8")
            .splitlines()
        )
        return files_in_diff
    else:
        merge_base = (
            subprocess.check_output(["git", "merge-base", target, source or "HEAD"])
            .decode("utf-8")
            .strip()
        )
        files_in_diff = (
            subprocess.check_output(
                ["git", "diff", "--diff-filter=d", "--name-only", merge_base]
                + ([source] if source else [])
            )
            .decode("utf-8")
            .splitlines()
        )
        return files_in_diff


def main():
    options = parse_args()
    check_clang_format()
    cpp_files = find_cpp_files("src/workerd")
    if options.subcommand == "git":
        files_in_diff = git_get_modified_files(
            options.target, options.source, options.staged
        )
        cpp_files = list(set(cpp_files) & set(files_in_diff))

    clang_format(cpp_files, options.check)

    # TODO: lint js, ts, bazel files


if __name__ == "__main__":
    main()
