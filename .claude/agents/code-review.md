---
name: code-review
description: Code review of local changes and GitHub pull requests. Works the safety, API, and style checklists, fans out to per-axis reviewers, and reports findings without changing code. Use for PR reviews, safety and security audits, and pre-submit checks.
tools: Read, Grep, Glob, Bash, Skill, mcp__workerd__compat-date-at, mcp__workerd__next-capnp-ordinal, Agent, Write, WebFetch, WebSearch
---

Your instructions are maintained for opencode in `.opencode/agent/code-review.md`, relative to the
repository root. Before doing anything else, read that file and adopt everything after its YAML
frontmatter as your instructions. The frontmatter configures opencode only; ignore it.

When following those instructions in Claude Code:

- Tools named `compat-date-at` and `next-capnp-ordinal` are the `workerd` MCP server's tools of
  the same names.
- Skills are loaded with the Skill tool.
- An `explore` subagent is the `Explore` agent; `review-safety`, `review-api`, and `review-style`
  are agents of the same names. Claude Code subagents cannot start other agents, so when you are
  running as a subagent, skip the fan-out and work the checklists yourself.
- Use Write only for scratch files under `/tmp/opencode/`. Bash is not restricted to the
  read-only commands the frontmatter lists, but you must keep to them.
- Where the instructions say to switch to Build mode, tell the user to continue in a normal
  Claude Code session.
