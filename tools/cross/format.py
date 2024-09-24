#!/usr/bin/env python3

import logging
import os
import platform
import re
import subprocess
import sys
from argparse import ArgumentParser, Namespace
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from sys import exit
from typing import Callable, Optional

CLANG_FORMAT = os.environ.get("CLANG_FORMAT", "clang-format")
PRETTIER = os.environ.get(
    "PRETTIER", "bazel-bin/node_modules/prettier/bin/prettier.cjs"
)


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
        help=(
            "consider files modified in the specified commit-ish; "
            "if not specified, defaults to all changes in the working directory"
        ),
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
            "--staged cannot be used with --source or --target; "
            "use --staged with --source=HEAD"
        )
        exit(1)
    return options


def check_clang_format() -> None:
    try:
        # Run clang-format with --version to check its version
        output = subprocess.check_output([CLANG_FORMAT, "--version"], encoding="utf-8")
        match = re.search(r"version\s*(\d+)\.(\d+)\.(\d+)", output)
        if not match:
            logging.error("unable to read clang version")
            exit(1)

        major, minor, patch = match.groups()
        if int(major) != 18 or int(minor) != 1 or int(patch) != 8:
            logging.error("clang-format version must be 18.1.8")
            exit(1)
    except FileNotFoundError:
        # Clang-format is not in the PATH
        logging.exception("clang-format not found in the PATH")
        exit(1)


def filter_files_by_globs(
    files: list[Path], dir_path: Path, globs: tuple[str, ...]
) -> list[Path]:
    return [
        file
        for file in files
        if file.is_relative_to(dir_path) and matches_any_glob(globs, file)
    ]


def matches_any_glob(globs: tuple[str, ...], file: Path) -> bool:
    return any(file.match(glob) for glob in globs)


def exec_target() -> str:
    ALIASES = {"aarch64": "arm64", "x86_64": "amd64", "AMD64": "amd64"}

    machine = platform.machine()
    return f"{sys.platform}-{ALIASES.get(machine, machine)}"


def init_external_dir() -> Path:
    # Create a symlink to the bazel external directory
    external_dir = Path("external")

    # Fast path to avoid calling into bazel
    if external_dir.exists():
        return external_dir

    try:
        bazel_base = Path(
            subprocess.run(["bazel", "info", "output_base"], capture_output=True)
            .stdout.decode()
            .strip()
        )
        external_dir.symlink_to(bazel_base / "external")
    except FileExistsError:
        # It's possible the link was created while we were working; this is fine
        pass

    return external_dir


def run_bazel_tool(
    tool_name: str, args: list[str], is_archive: bool = False
) -> subprocess.CompletedProcess:
    tool_target = f"{tool_name}-{exec_target()}"

    if is_archive:
        tool_path = init_external_dir() / tool_target / tool_name
    else:
        tool_path = init_external_dir() / tool_target / "file" / "downloaded"

    if not tool_path.exists():
        fetch_target = (
            f"@{tool_target}//:file" if is_archive else f"@{tool_target}//file"
        )
        subprocess.run(["bazel", "fetch", fetch_target])

    return subprocess.run([tool_path, *args])


def clang_format(files: list[Path], check: bool = False) -> bool:
    check_clang_format()
    cmd = [CLANG_FORMAT]
    if check:
        cmd += ["--dry-run", "--Werror"]
    else:
        cmd.append("-i")
    result = subprocess.run(cmd + files)
    return result.returncode == 0


def prettier(files: list[Path], check: bool = False) -> bool:
    if not Path(PRETTIER).exists():
        subprocess.run(["bazel", "build", "//:node_modules/prettier"])

    cmd = [PRETTIER, "--log-level=warn", "--check" if check else "--write"]
    result = subprocess.run(cmd + files)
    return result.returncode == 0


def buildifier(files: list[Path], check: bool = False) -> bool:
    cmd = ["--mode=check" if check else "--mode=fix"]
    result = run_bazel_tool("buildifier", cmd + files)
    return result.returncode == 0


def ruff(files: list[Path], check: bool = False) -> bool:
    if not files:
        return True

    cmd = ["check"]
    if not check:
        cmd.append("--fix")
    result1 = run_bazel_tool("ruff", cmd + files, is_archive=True)

    # format
    cmd = ["format"]
    if check:
        cmd.append("--diff")

    result2 = run_bazel_tool("ruff", cmd + files, is_archive=True)
    return result1.returncode == 0 and result2.returncode == 0


def git_get_modified_files(
    target: str, source: Optional[str], staged: bool
) -> list[Path]:
    if staged:
        files_in_diff = subprocess.check_output(
            ["git", "diff", "--diff-filter=d", "--name-only", "--cached"],
            encoding="utf-8",
        ).splitlines()
        return [Path(file) for file in files_in_diff]
    else:
        merge_base = subprocess.check_output(
            ["git", "merge-base", target, source or "HEAD"], encoding="utf-8"
        ).strip()
        files_in_diff = subprocess.check_output(
            ["git", "diff", "--diff-filter=d", "--name-only", merge_base]
            + ([source] if source else []),
            encoding="utf-8",
        ).splitlines()
        return [Path(file) for file in files_in_diff]


def git_get_all_files() -> list[Path]:
    files = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        encoding="utf-8",
    ).splitlines()
    return [Path(file) for file in files]


@dataclass
class FormatConfig:
    directory: str
    globs: tuple[str, ...]
    formatter: Callable[[list[Path], bool], bool]


FORMATTERS = [
    # FormatConfig(
    #     directory="src/workerd", globs=("*.c++", "*.h"), formatter=clang_format
    # ),
    FormatConfig(
        directory="src",
        globs=("*.js", "*.ts", "*.cjs", "*.ejs", "*.mjs"),
        formatter=prettier,
    ),
    FormatConfig(directory="src", globs=("*.json",), formatter=prettier),
    FormatConfig(directory=".", globs=("*.py",), formatter=ruff),
    FormatConfig(
        directory=".",
        globs=("*.bzl", "WORKSPACE", "BUILD", "BUILD.*"),
        formatter=buildifier,
    ),
]


def format(config: FormatConfig, files: list[Path], check: bool) -> tuple[bool, str]:
    matching_files = filter_files_by_globs(files, Path(config.directory), config.globs)

    if not matching_files:
        return (
            True,
            f"No matching files for {config.directory} ({', '.join(config.globs)})",
        )

    result = config.formatter(matching_files, check)
    message = (
        f"{len(matching_files)} files in {config.directory} ({', '.join(config.globs)})"
    )
    return (
        result,
        f"{'Checked' if check else 'Formatted'} {message}",
    )


def main() -> None:
    options = parse_args()
    if options.subcommand == "git":
        files = git_get_modified_files(options.target, options.source, options.staged)
    else:
        files = git_get_all_files()

    all_ok = True

    with ThreadPoolExecutor() as executor:
        future_to_config = {
            executor.submit(format, config, files, options.check): config
            for config in FORMATTERS
        }
        for future in as_completed(future_to_config):
            config = future_to_config[future]
            try:
                result, message = future.result()
                all_ok &= result
                logging.info(message)
            except Exception:
                logging.exception(
                    f"Formatter for {config.directory} generated an exception"
                )
                all_ok = False

    if not all_ok:
        logging.error(
            "Code has linting issues. Fix with python ./tools/cross/format.py"
        )
        exit(1)


if __name__ == "__main__":
    main()
