---
name: investigation-notes
description: Structured scratch tracking document for investigation state during bug hunts - prevents re-reading code, losing context, and rabbit holes; maintains external memory so you don't re-derive conclusions
---

# Investigation Notes

## Overview

During bug investigations, maintain a lightweight scratch document as external memory. This prevents
re-reading code you've already analyzed, losing track of which hypothesis you're testing, and
silently drifting into rabbit holes.

**Core principle:** Write it down once, refer to it later. Re-reading your one-line note is faster
than re-reading 200 lines of source.

**Always** keep the document up to date with your current focus, hypotheses, and learnings from
code reads and tests.

**Always** refer to the document before re-reading code or forming a new hypothesis. If the
information is there, use it. If it's not sufficient, read the code, then write a better note.

**The document never takes priority over writing or running a test.** If you're choosing between
updating notes and writing a test, write the test. Update the notes after.

## The Document

Create `~/tmp/investigate-<short-name>.md` during orientation (step 2 of `/investigate`).

The short name should be descriptive enough to identify the investigation (e.g.,
`investigate-concurrent-write.md`, `investigate-pipe-zombie-state.md`).

Once created, notify the user and provide the file path so they can open it in their editor.

### Format

```markdown
# Investigation: <one-line bug description>

## Error

<assertion/crash message, file:line — written once, never changes>

## Current Focus

<single sentence: what you are doing RIGHT NOW>

## Hypotheses

1. [TESTING] <one sentence> — test: <test name or "not yet written">
2. [REJECTED] <one sentence> — disproved by: <one sentence>
3. [CONFIRMED] <one sentence> — evidence: <test name + result>

## Code Read

- `file:line-range` — <what you learned, short paragraph or bullet>

## Tests

- `file:line` "test name" — <result + what it means, one sentence>

## Ruled Out

- <thing you investigated and eliminated, one sentence why>

## Next

1. <concrete next action>
2. <fallback>
```

## Creation

**Create the document when:**

- You have more than one hypothesis to track
- You've read more than 3 files/functions
- You're about to re-read code you already read
- The investigation is going to span multiple iterations of test-write-run

When created, populate Error, Current Focus, your current hypotheses, and anything you've
already learned (backfill "Code Read" and "Tests" from what you've done so far).

## Rules

- **Always** update "Current Focus" before starting any new thread of work
- **Always** Add a hypothesis for what you're doing
- **Always** update the document after each significant action:
  - After reading a function or file section → add to "Code Read"
  - After forming or rejecting a hypothesis → update "Hypotheses"
  - **After running a test → update BEFORE doing anything else.** Record: what test ran, what the
    result was, what it means for the active hypothesis. This is non-negotiable — it takes 30
    seconds and prevents the pattern of running multiple tests, losing track of what they proved,
    and falling back to code reading.
  - After starting a new thread of work → update "Current Focus"
- **Do not** dump large amounts of code into the document. Instead, reference it by `file:line`.
- **Do not reorganize or reformat the document** - this is a scratchpad, not a report.
- **You may** include brief, simple diagrams if they help you understand and retain information.

## Hypothesis Limits

- **Maximum 3 active hypotheses at a time.** If you want to add a fourth, reject or merge one first.
- **Maximum 1 `[TESTING]` at a time.** Commit to one, test it, resolve it, then move on.
- **Every hypothesis must have a test or a concrete plan to write one.** "Need to investigate
  further" is not a valid state — that's analysis paralysis wearing a label.

Valid statuses:

- `[UNTESTED]` — formed but not yet tested. Must become `[TESTING]` or `[REJECTED]` soon.
- `[TESTING]` — actively being tested. Only one at a time.
- `[CONFIRMED]` — test reproduced the bug as predicted.
- `[REJECTED]` — test disproved it, or evidence rules it out. Include why.
- `[SUPERSEDED]` — replaced by a more specific hypothesis. Reference the replacement.
