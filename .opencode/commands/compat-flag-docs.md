---
description: Create docs PR in cloudflare-docs for new compat flags on this branch
---

Create documentation for new compatibility flags that have default-on dates
(`$compatEnableDate` or `$impliedByAfterDate`) added on the current branch
but are not yet documented in cloudflare-docs.

**Arguments:** $ARGUMENTS

## Argument handling

- If an argument is provided, treat it as the name of a single flag to document.
  The argument can be the enable-flag name (e.g. `websocket_close_reason_byte_limit`)
  or the hyphenated slug form (e.g. `websocket-close-reason-byte-limit`).
  Look up that flag in `src/workerd/io/compatibility-date.capnp`, verify it has a
  `$compatEnableDate` or `$impliedByAfterDate`, and create docs for it only.
  If the flag is not found or has no default-on date, report an error.
- If no argument is provided, discover all new flags on the current branch
  (compared to `origin/main`) and create docs for every undocumented one.

## Context

The workerd CI check (`.github/workflows/compat-flag-docs.yml`) blocks PRs that add
compat flags with default-on dates unless a matching docs PR exists in
`cloudflare/cloudflare-docs` with at least one approving review.

The cloudflare-docs repo should be checked out locally at
`~/projects/cloudflare/cloudflare-docs`. If it isn't there, ask the user
for the correct path before proceeding.

## Steps

### 1. Identify flags that need docs

**Single-flag mode** (argument provided):

1. Normalize the argument: if it contains hyphens, convert to underscores to
   get the enable-flag name. Also keep the hyphenated form as the slug.
2. Read `src/workerd/io/compatibility-date.capnp` and find the field whose
   `$compatEnableFlag` matches the flag name.
3. If the flag has `$experimental`, tell the user experimental flags are
   skipped by the CI check and don't need docs (but they can still create
   docs if desired -- ask).
4. Verify it has `$compatEnableDate` or `$impliedByAfterDate`. If not, tell
   the user this flag has no default-on date and doesn't need docs for the CI
   check (but they can still create docs if desired -- ask).
5. Proceed with this single flag.

**All-flags mode** (no argument):

Compare `src/workerd/io/compatibility-date.capnp` on the current branch against
`origin/main` to find flags that:

- Were added with a `$compatEnableDate(...)` annotation, OR
- Were added with a `$impliedByAfterDate(...)` annotation, OR
- Already existed but gained one of those annotations

For each such flag, extract:

- The `$compatEnableFlag("name")` value (the enable flag name)
- The `$compatDisableFlag("name")` value if present
- The `$compatEnableDate("YYYY-MM-DD")` value if present
- The `$impliedByAfterDate(name = "...", date = "YYYY-MM-DD")` info if present
- The comment block describing the flag's behavior
  Skip any `obsolete*` fields and any flags with `$experimental`.

### 2. Check which flags are already documented

For each flag, convert its enable-flag name to a slug (replace `_` with `-`).
Check whether `<slug>.md` exists in the cloudflare-docs checkout at
`src/content/compatibility-flags/`. If it already exists, skip it (and tell
the user it's already documented).

If every flag is already documented, report that and stop.

### 3. Create the doc files

For each undocumented flag, create a markdown file in the cloudflare-docs
checkout at `src/content/compatibility-flags/<slug>.md`.

Use the following template, matching the existing conventions in that directory:

```markdown
---
_build:
  publishResources: false
  render: never
  list: never

name: '<Human-readable description of the flag>'
sort_date: '<YYYY-MM-DD>'
enable_date: '<YYYY-MM-DD>'
enable_flag: '<enable_flag_name>'
disable_flag: '<disable_flag_name>'
---

<Description of what the flag does, derived from the capnp comment block.
Mention the enable_flag name in backticks. Describe the old behavior and
what changes when the flag is enabled. Link to relevant docs pages or
specs where appropriate.>
```

Rules for the frontmatter:

- `sort_date` and `enable_date` should be the same date
- For flags with only `$impliedByAfterDate` (no `$compatEnableDate`), use the
  date from the `$impliedByAfterDate` annotation
- If a flag has both `$compatEnableDate` and `$impliedByAfterDate`, use the
  `$compatEnableDate` value
- If a flag has no `$compatDisableFlag`, omit the `disable_flag` field
- `name` should be a concise human-readable description (not the flag name)

### 4. Create a branch and PR

In the cloudflare-docs checkout:

1. Before proceeding, present the draft changes to the use and ask for confirmation to continue.
   If the user declines, you must stop here.
2. Make sure you're on an up-to-date `production` branch first (`git fetch origin`)
3. Create a new branch named `compat-flag-docs/<workerd-branch-name>` (use the
   current workerd branch name)
4. Stage and commit all the new doc files with a message like:
   `docs: add compatibility flag documentation for <flag-names>`
5. Push the branch
6. Open a PR with `gh pr create` targeting the `production` branch:
   - Title: `docs: add compatibility flag documentation`
   - Body should list which flags are documented and link to the workerd PR
     that adds them (use `!`gh pr view --json url -q .url`` to get the
     workerd PR URL, or ask the user if there's no current PR)

### 5. Report

Print a summary:

- How many flags were found that needed docs
- The files that were created
- The URL of the cloudflare-docs PR
