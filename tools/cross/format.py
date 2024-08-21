#!/usr/bin/env python3

import logging
import os
import re
import shutil
import subprocess
from argparse import ArgumentParser, Namespace
from typing import List, Optional, Tuple, Callable
from dataclasses import dataclass


CLANG_FORMAT = os.environ.get("CLANG_FORMAT", "clang-format")
PRETTIER = os.environ.get("PRETTIER", "node_modules/.bin/prettier")
RUFF = os.environ.get("RUFF", "ruff")


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
    if (
        options.subcommand == "git"
        and options.staged
        and (options.source is not None or options.target != "HEAD")
    ):
        logging.error(
            "--staged cannot be used with --source or --target; use --staged with --source=HEAD"
        )
        exit(1)
    return options


def check_clang_format() -> bool:
    try:
        # Run clang-format with --version to check its version
        output = subprocess.check_output([CLANG_FORMAT, "--version"], encoding="utf-8")
        major, _, _ = re.search(r"version\s*(\d+)\.(\d+)\.(\d+)", output).groups()
        if int(major) != 18:
            logging.error("clang-format version must be 18")
            exit(1)
    except FileNotFoundError:
        # Clang-format is not in the PATH
        logging.error("clang-format not found in the PATH")
        exit(1)


def filter_files_by_exts(
    files: List[str], dir_path: str, exts: Tuple[str, ...]
) -> List[str]:
    return [
        file
        for file in files
        if (dir_path == "." or file.startswith(dir_path + "/")) and file.endswith(exts)
    ]


def clang_format(files: List[str], check: bool = False) -> bool:
    cmd = [CLANG_FORMAT]
    if check:
        cmd += ["--dry-run", "--Werror"]
    else:
        cmd.append("-i")
    result = subprocess.run(cmd + files)
    return result.returncode == 0


def prettier(files: List[str], check: bool = False) -> bool:
    cmd = [PRETTIER, "--log-level=warn"]
    if check:
        cmd.append("--check")
    else:
        cmd.append("--write")
    result = subprocess.run(cmd + files)
    return result.returncode == 0


def ruff(files: List[str], check: bool = False) -> bool:
    if files and not shutil.which(RUFF):
        msg = "Cannot find ruff, will not format Python"
        if check:
            # In ci, fail.
            logging.error(msg)
            return False
        else:
            # In a local checkout, let it go. If the user wants Python
            # formatting they can install ruff and run again.
            logging.warning(msg)
            return True
    cmd = [RUFF, "format"]
    if check:
        cmd.append("--diff")
    result = subprocess.run(cmd + files)
    return result.returncode == 0


def git_get_modified_files(
    target: str, source: Optional[str], staged: bool
) -> List[str]:
    if staged:
        files_in_diff = subprocess.check_output(
            ["git", "diff", "--diff-filter=d", "--name-only", "--cached"],
            encoding="utf-8",
        ).splitlines()
        return files_in_diff
    else:
        merge_base = subprocess.check_output(
            ["git", "merge-base", target, source or "HEAD"], encoding="utf-8"
        ).strip()
        files_in_diff = subprocess.check_output(
            ["git", "diff", "--diff-filter=d", "--name-only", merge_base]
            + ([source] if source else []),
            encoding="utf-8",
        ).splitlines()
        return files_in_diff


def git_get_all_files() -> List[str]:
    return subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        encoding="utf-8",
    ).splitlines()


@dataclass
class FormatConfig:
    directory: str
    extensions: Tuple[str, ...]
    formatter: Callable[[List[str], bool], bool]


FORMATTERS = [
    FormatConfig(
        directory="src/workerd", extensions=(".c++", ".h"), formatter=clang_format
    ),
    FormatConfig(
        directory="src",
        extensions=(".js", ".ts", ".cjs", ".ejs", ".mjs", ".json"),
        formatter=prettier,
    ),
    FormatConfig(directory=".", extensions=(".py",), formatter=ruff),
    # TODO: lint bazel files
]


def format(config: FormatConfig, files: List[str], check: bool) -> bool:
    matching_files = filter_files_by_exts(files, config.directory, config.extensions)

    if not matching_files:
        return True

    return config.formatter(matching_files, check)


def main():
    options = parse_args()
    check_clang_format()
    if options.subcommand == "git":
        files = set(
            git_get_modified_files(options.target, options.source, options.staged)
        )
    else:
        files = git_get_all_files()

    all_ok = True

    for config in FORMATTERS:
        all_ok &= format(config, files, options.check)

    if not all_ok:
        logging.error(
            "Code has linting issues. Fix with python ./tools/cross/format.py"
        )
        exit(1)


if __name__ == "__main__":
    main()
