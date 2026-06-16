#!/usr/bin/env python3
"""Merge clang-tidy config files for Bazel clang-tidy actions.

The first config is the base config. It is emitted with all of its settings,
except that its `Checks` and `CustomChecks` sections are replaced by merged
sections from every input config.

Additional configs only contribute:

* `Checks`: appended to the base checks. A leading `-*` from additional configs
  is ignored so that a reusable config cannot reset the primary config's checks.
  Duplicate checks are removed, preserving first occurrence.
* `CustomChecks`: appended verbatim after the base config's custom checks.

All other settings from additional configs are ignored. This lets workerd use
its own `WarningsAsErrors`, `HeaderFilterRegex`, `CheckOptions`, etc. while
still reusing custom checks defined by dependency configs like capnproto's.
"""

import re
import sys
from pathlib import Path

TOP_LEVEL_KEY = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\s*:")
BLOCK_SCALAR = re.compile(r":\s*[>|][1-9]?[+-]?\s*$")


def split_sections(lines):
    config = {
        "preamble": [],
        "sections": {},
    }
    current_key = None
    current_lines = []
    # Avoid treating key-looking lines inside YAML block scalars as new sections.
    block_scalar_indent = None
    pending_block_scalar_parent_indent = None

    for line in lines:
        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))

        if block_scalar_indent is not None:
            if not stripped or indent >= block_scalar_indent:
                current_lines.append(line)
                continue
            block_scalar_indent = None

        if pending_block_scalar_parent_indent is not None:
            if not stripped:
                current_lines.append(line)
                continue
            if indent > pending_block_scalar_parent_indent:
                block_scalar_indent = indent
                pending_block_scalar_parent_indent = None
                current_lines.append(line)
                continue
            pending_block_scalar_parent_indent = None

        match = TOP_LEVEL_KEY.match(line)
        if match:
            if current_key is None:
                config["preamble"].extend(current_lines)
            else:
                config["sections"][current_key] = current_lines
            current_key = match.group(1)
            current_lines = [line]
        else:
            current_lines.append(line)

        if BLOCK_SCALAR.search(strip_yaml_comment(line)):
            pending_block_scalar_parent_indent = indent

    if current_key is None:
        config["preamble"].extend(current_lines)
    else:
        config["sections"][current_key] = current_lines

    return config


def strip_yaml_comment(line):
    quote = None
    index = 0
    while index < len(line):
        char = line[index]
        if char in ("'", '"'):
            if (
                quote == "'"
                and char == "'"
                and index + 1 < len(line)
                and line[index + 1] == "'"
            ):
                index += 1
            elif quote == char:
                quote = None
            elif quote is None:
                quote = char
        elif quote == '"' and char == "\\":
            index += 1
        elif char == "#" and quote is None:
            return line[:index]
        index += 1
    return line


def parse_checks(section_lines, skip_reset):
    if not section_lines:
        return []

    first = section_lines[0]
    _, value = first.split(":", 1)
    first_value = value.strip()
    if first_value in (">", "|", ">-", "|-", ">+", "|+"):
        check_lines = section_lines[1:]
    else:
        check_lines = [first_value, *section_lines[1:]]

    checks = []
    for line in check_lines:
        for part in strip_yaml_comment(line).split(","):
            check = part.strip()
            if not check:
                continue
            if skip_reset and check == "-*":
                continue
            checks.append(check)
    return checks


def parse_custom_checks(section_lines):
    if not section_lines:
        return []

    return [line.rstrip("\n") for line in section_lines[1:]]


def emit_checks(checks):
    if not checks:
        return []

    lines = ["Checks: >\n"]
    for index, check in enumerate(checks):
        suffix = "," if index + 1 < len(checks) else ""
        lines.append(f"  {check}{suffix}\n")
    return lines


def emit_custom_checks(custom_checks):
    if not custom_checks:
        return []

    return ["CustomChecks:\n"] + [line + "\n" for line in custom_checks]


def merge_configs(config_paths):
    parsed_configs = []
    for path in config_paths:
        with Path(path).open(encoding="utf-8") as config:
            parsed_configs.append(split_sections(config.readlines()))

    first_config = parsed_configs[0]
    checks = []
    custom_checks = []

    for index, config in enumerate(parsed_configs):
        sections = config["sections"]
        checks.extend(parse_checks(sections.get("Checks", []), skip_reset=index > 0))
        custom_checks.extend(parse_custom_checks(sections.get("CustomChecks", [])))

    checks = list(dict.fromkeys(checks))

    output = []
    inserted_checks = False
    output.extend(first_config["preamble"])

    for key, lines in first_config["sections"].items():
        if key == "Checks":
            output.extend(emit_checks(checks))
            output.append("\n")
            output.extend(emit_custom_checks(custom_checks))
            if custom_checks:
                output.append("\n")
            inserted_checks = True
        elif key != "CustomChecks":
            output.extend(lines)

    if not inserted_checks:
        output.extend(emit_checks(checks))
        if checks:
            output.append("\n")
        output.extend(emit_custom_checks(custom_checks))

    return output


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: merge_clang_tidy_configs.py OUTPUT CONFIG [CONFIG ...]")

    output_path = sys.argv[1]
    config_paths = sys.argv[2:]
    with Path(output_path).open("w", encoding="utf-8") as output:
        output.writelines(merge_configs(config_paths))


if __name__ == "__main__":
    main()
