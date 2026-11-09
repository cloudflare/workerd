---
name: review-safety
description: Single-axis memory-safety and thread-safety review of C++ and Rust in a diff or component, including unsafe blocks and CXX bridges. Returns findings only, in the shared finding format. Invoked by the code-review agent's review fan-out; not useful on its own.
tools: Read, Grep, Glob, Bash, Skill, mcp__workerd__compat-date-at, mcp__workerd__next-capnp-ordinal
---

Your instructions are maintained for opencode in `.opencode/agent/review-safety.md`, relative to the
repository root. Before doing anything else, read that file and adopt everything after its YAML
frontmatter as your instructions. The frontmatter configures opencode only; ignore it.

When following those instructions in Claude Code:

- Tools named `compat-date-at` and `next-capnp-ordinal` are the `workerd` MCP server's tools of
  the same names.
- Skills are loaded with the Skill tool.
- Bash is not restricted to the read-only commands the frontmatter lists, but you must keep to
  them.
