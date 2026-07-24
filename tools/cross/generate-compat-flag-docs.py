#!/usr/bin/env python3
"""
Tool to automatically generate documentation for compatibility flags.

Reads compatibility flags from src/workerd/io/compatibility-date.capnp
and generates markdown documentation files in the cloudflare-docs format.

Skips:
- Experimental flags (marked with $experimental)
- Obsolete flags (field name starts with "obsolete")
- Flags that already have documentation
"""

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import yaml


@dataclass
class CompatFlag:
    """Represents a compatibility flag."""

    field_name: str
    enable_flag: Optional[str]
    disable_flag: Optional[str]
    enable_date: Optional[str]
    implied_by_after_date: Optional[str]
    description: str
    is_experimental: bool
    is_obsolete: bool

    def get_doc_filename(self) -> Optional[str]:
        """Generate the documentation filename from the enable flag name."""
        if not self.enable_flag:
            return None
        # Convert flag name to kebab-case filename
        return f"{self.enable_flag.replace('_', '-')}.md"

    def get_human_readable_name(self) -> str:
        """Generate a human-readable name from the enable flag."""
        if not self.enable_flag:
            return self.field_name
        # Convert snake_case to Title Case
        words = self.enable_flag.replace("_", " ").split()
        return " ".join(word.capitalize() for word in words)

    def should_generate_docs(self, documented_flags: set[str]) -> bool:
        """Check if documentation should be generated for this flag."""
        if self.is_experimental:
            return False
        if self.is_obsolete:
            return False
        if not self.enable_flag:
            return False

        # Skip if flag has neither an enable date nor an implied by after date
        if not self.enable_date and not self.implied_by_after_date:
            return False

        # Check if this flag is already documented
        if self.enable_flag in documented_flags:
            return False

        return True


def parse_capnp_file(filepath: Path) -> list[CompatFlag]:
    """Parse the compatibility-date.capnp file and extract flags."""
    content = filepath.read_text()

    flags = []

    # Find all field definitions
    # Pattern: field_name @number :Bool followed by annotations and comments
    pattern = r"(\w+)\s+@\d+\s*:Bool\s*([^;]*);"

    matches = re.finditer(pattern, content, re.MULTILINE | re.DOTALL)

    for match in matches:
        field_name = match.group(1)
        annotations_block = match.group(2)

        # Check if obsolete
        is_obsolete = field_name.startswith("obsolete")

        # Extract annotations
        enable_flag = None
        disable_flag = None
        enable_date = None
        implied_by_after_date = None
        is_experimental = False

        # Extract enable flag
        enable_match = re.search(r'\$compatEnableFlag\("([^"]+)"\)', annotations_block)
        if enable_match:
            enable_flag = enable_match.group(1)

        # Extract disable flag
        disable_match = re.search(
            r'\$compatDisableFlag\("([^"]+)"\)', annotations_block
        )
        if disable_match:
            disable_flag = disable_match.group(1)

        # Extract enable date
        date_match = re.search(r'\$compatEnableDate\("([^"]+)"\)', annotations_block)
        if date_match:
            enable_date = date_match.group(1)

        # Extract implied by after date
        implied_match = re.search(
            r'\$impliedByAfterDate\([^)]*date\s*=\s*"([^"]+)"', annotations_block
        )
        if implied_match:
            implied_by_after_date = implied_match.group(1)

        # Check if experimental
        if "$experimental" in annotations_block:
            is_experimental = True

        # Extract description from comments after the field
        # Find the position after this field's semicolon
        field_end = match.end()

        # Look forward for comments
        after_field = content[field_end:]
        lines_after = after_field.split("\n")

        # Collect comment lines
        description_lines = []
        for line in lines_after:
            stripped = line.strip()
            if stripped.startswith("#"):
                # Remove the leading # and whitespace
                comment_text = stripped[1:].strip()
                description_lines.append(comment_text)
            elif stripped == "":
                # Empty line - stop if we've already collected comments
                if description_lines:
                    break
                # Otherwise continue looking
                continue
            else:
                # Non-comment, non-empty line - stop
                break

        description = " ".join(description_lines) if description_lines else ""

        flag = CompatFlag(
            field_name=field_name,
            enable_flag=enable_flag,
            disable_flag=disable_flag,
            enable_date=enable_date,
            implied_by_after_date=implied_by_after_date,
            description=description,
            is_experimental=is_experimental,
            is_obsolete=is_obsolete,
        )

        flags.append(flag)

    return flags


def parse_frontmatter(filepath: Path) -> dict[str, str]:
    """Parse the YAML frontmatter from a markdown file."""
    try:
        content = filepath.read_text()

        # Extract frontmatter between --- markers
        match = re.match(r"^---\s*\n(.*?)\n---\s*\n", content, re.DOTALL)
        if not match:
            return {}

        frontmatter_text = match.group(1)

        # Parse YAML
        try:
            frontmatter = yaml.safe_load(frontmatter_text)
            return frontmatter if isinstance(frontmatter, dict) else {}
        except yaml.YAMLError:
            return {}
    except Exception:
        return {}


def get_existing_docs(docs_dir: str) -> set[str]:
    """
    Get the set of compatibility flags that are already documented.

    Returns a set of enable_flag and disable_flag values found in existing docs.
    """
    docs_path = Path(docs_dir)
    if not docs_path.exists():
        return set()

    documented_flags = set()

    for filepath in docs_path.iterdir():
        if not filepath.is_file() or filepath.suffix != ".md":
            continue

        frontmatter = parse_frontmatter(filepath)

        # Add both enable_flag and disable_flag if present
        if "enable_flag" in frontmatter:
            documented_flags.add(frontmatter["enable_flag"])
        if "disable_flag" in frontmatter:
            documented_flags.add(frontmatter["disable_flag"])

    return documented_flags


def generate_markdown(flag: CompatFlag) -> str:
    """Generate markdown documentation for a flag."""

    # Prepare the frontmatter
    frontmatter = [
        "---",
        "_build:",
        "  publishResources: false",
        "  render: never",
        "  list: never",
        "",
        f'name: "{flag.get_human_readable_name()}"',
    ]

    if flag.enable_date:
        frontmatter.append(f'sort_date: "{flag.enable_date}"')

    if flag.enable_flag:
        frontmatter.append(f'enable_flag: "{flag.enable_flag}"')

    if flag.disable_flag:
        frontmatter.append(f'disable_flag: "{flag.disable_flag}"')

    frontmatter.append("---")

    # Prepare the body
    body = []

    if flag.description:
        body.append(flag.description)
    else:
        body.append(f"Compatibility flag: `{flag.enable_flag}`")

    # Combine everything
    return "\n".join(frontmatter) + "\n\n" + "\n".join(body) + "\n"


def main():
    """Main entry point."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate compatibility flag documentation"
    )
    parser.add_argument(
        "--capnp-file",
        default="src/workerd/io/compatibility-date.capnp",
        help="Path to compatibility-date.capnp file",
        type=Path,
    )
    parser.add_argument(
        "--docs-dir",
        default="../cloudflare-docs/src/content/compatibility-flags",
        help="Path to cloudflare-docs compatibility-flags directory",
        type=Path,
    )
    parser.add_argument(
        "--output-dir", help="Output directory (defaults to --docs-dir)", type=Path
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print what would be generated without writing files",
    )
    parser.add_argument(
        "--force", action="store_true", help="Generate docs even if they already exist"
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Show detailed information about flag detection",
    )

    args = parser.parse_args()

    output_dir = args.output_dir or args.docs_dir

    # Parse the capnp file
    print(f"Parsing {args.capnp_file}...")
    flags = parse_capnp_file(args.capnp_file)
    print(f"Found {len(flags)} total flags")

    # Get existing docs
    documented_flags = get_existing_docs(args.docs_dir) if not args.force else set()
    print(f"Found {len(documented_flags)} already-documented compatibility flags")

    if args.verbose and documented_flags:
        print("\nAlready documented flags:")
        for flag_name in sorted(documented_flags):
            print(f"  - {flag_name}")

    # Filter flags that need documentation
    flags_to_document = [
        f for f in flags if f.should_generate_docs(documented_flags) or args.force
    ]

    print(f"\nFlags to document: {len(flags_to_document)}")
    print(f"Skipped experimental: {sum(1 for f in flags if f.is_experimental)}")
    print(f"Skipped obsolete: {sum(1 for f in flags if f.is_obsolete)}")
    print(
        f"Skipped without dates: {sum(1 for f in flags if not f.is_experimental and not f.is_obsolete and f.enable_flag and not f.enable_date and not f.implied_by_after_date)}"
    )
    print(
        f"Already documented: {sum(1 for f in flags if not f.is_experimental and not f.is_obsolete and f.enable_flag and f.enable_flag in documented_flags)}"
    )
    print(
        f"Missing enable flag: {sum(1 for f in flags if not f.enable_flag and not f.is_obsolete)}"
    )

    if not flags_to_document:
        print("\nNo flags to document!")
        return

    # Generate documentation
    print(f"\n{'Would generate' if args.dry_run else 'Generating'} documentation for:")

    for flag in flags_to_document:
        filename = flag.get_doc_filename()
        if not filename:
            continue

        filepath = output_dir / filename
        markdown = generate_markdown(flag)

        print(f"  - {filename}")

        if args.dry_run:
            print(f"\n--- {filename} ---")
            print(markdown)
            print("--- End ---\n")
        else:
            output_dir.mkdir(parents=True, exist_ok=True)
            filepath.write_text(markdown)

    if not args.dry_run:
        print(
            f"\nGenerated {len(flags_to_document)} documentation files in {output_dir}"
        )
    else:
        print(
            f"\nDry run complete. Would have generated {len(flags_to_document)} files."
        )


if __name__ == "__main__":
    main()
