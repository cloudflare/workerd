---
name: parent-project-skills
description: Bootstrap skill for discovering additional skills and context from a parent project when workerd is used as a submodule. Load this skill when tasks span project boundaries (e.g., Sentry/production investigation, integration testing, cross-repo debugging).
---

## Parent Project Skill Discovery

This project (workerd) may be embedded as a submodule within a larger project. When your task requires context beyond workerd itself — such as investigating production issues, writing integration tests, or understanding deployment architecture — you should check for additional skills and context in the parent project.

### Discovery procedure

1. Determine if a parent project exists by checking for a git root above the workerd directory:
   - Look for `../../.git` or `../../AGENTS.md` relative to the workerd root
   - If found, the parent project root is `../../` (two levels up from workerd)

2. Check for parent project skills:
   - Look for `../../.opencode/skills/*/SKILL.md`
   - Read any `../../AGENTS.md` or `../../AGENTS.md` for project-wide context

3. Load relevant skills by reading the SKILL.md files. These are not registered in the `skill` tool — read them directly with the Read tool.

4. Check for submodule-specific context:
   - Look for `../../.opencode/skills/*/SKILL.md` files that may reference workerd specifically

### Information boundary — CRITICAL

**workerd is an open-source project. The parent project is typically proprietary/internal.**

You MUST enforce a strict one-way information boundary:

- **ALLOWED**: Internal context informs your reasoning, helps you understand code paths, guides your investigation
- **NEVER**: Write internal details into workerd files — this includes:
  - Code comments referencing internal systems, services, or architecture
  - Commit messages mentioning internal projects, Sentry issues, Jira tickets, or internal URLs
  - PR descriptions or GitHub issue comments containing internal context
  - AGENTS.md, AGENTS.md, or documentation updates with internal knowledge
  - Variable names, error messages, or log strings that reveal internal details
  - Test files that encode internal architecture assumptions

When working across the boundary, frame all workerd-side artifacts in terms of workerd's own public concepts (Workers, Durable Objects, isolates, IoContext, etc.) without referencing how the parent project orchestrates them.

### When to use this skill

- Investigating production Sentry issues that involve workerd code
- Writing integration tests (e.g., ew-test) that live in the parent project
- Understanding how workerd APIs behave in the production deployment context
- Debugging issues that cross the workerd / parent-project boundary
- Reviewing code changes that affect both projects
- Planning new feature development that requires coordination between workerd and the parent project
