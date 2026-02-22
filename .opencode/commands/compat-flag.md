---
description: Look up a compatibility flag, or list all flags if no argument given
subtask: true
---

Look up compatibility flag: $ARGUMENTS

**If no argument is provided (empty or blank), list all compatibility flags:**

1. Read `src/workerd/io/compatibility-date.capnp` and find all fields with `$compatEnableFlag`.
2. For each flag, extract: field name, field number, enable flag string, enable date (or "experimental" / "no date"), and a one-line summary from the comment.
3. Output a summary table grouped by category (streams, nodejs, containers, general, etc.):

   | Flag        | Enable date | Description       |
   | ----------- | ----------- | ----------------- |
   | `flag_name` | 2025-01-15  | Brief description |

   Include the total count of compatibility flags.

**If an argument is provided, look up that specific flag:**

1. **Find the flag definition.** Search `src/workerd/io/compatibility-date.capnp` for the flag name. The argument may be the snake_case flag name (e.g., `text_decoder_replace_surrogates`) or the camelCase field name (e.g., `textDecoderReplaceSurrogates`). Try both forms.

2. **Extract flag metadata** from the capnp definition:
   - Field name and number
   - `$compatEnableFlag` name
   - `$compatDisableFlag` name (if present)
   - `$compatEnableDate` (if set)
   - Whether it has `$experimental` annotation
   - The comment block describing the flag

3. **Find C++ usage sites.** Search for the getter (e.g., `getTextDecoderReplaceSurrogates()`) across the codebase:

   ```
   grep -rn "getTextDecoderReplaceSurrogates\|text_decoder_replace_surrogates" src/
   ```

4. **Find tests.** Search for the snake_case flag name in `.wd-test` and test `.js` files:

   ```
   grep -rn "text_decoder_replace_surrogates" src/ --include='*.wd-test' --include='*-test.js' --include='*-test.ts'
   ```

5. **Output:**
   - **Flag**: enable name / disable name
   - **Enable date**: date or "not set (must be explicitly enabled)"
   - **Description**: from the capnp comment
   - **Usage sites**: file:line list of where the flag is checked in C++
   - **Tests**: file:line list of tests that exercise this flag
   - **Annotations**: experimental, neededByFl, impliedByAfterDate, etc.
   - If the flag is not found, suggest checking for typos and list flags with similar names
