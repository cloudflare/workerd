---
description: Look up a compatibility flag, or list all flags if no argument given
subtask: true
---

Look up compatibility flag: $ARGUMENTS

**If no argument is provided (empty or blank), list all compatibility flags:**

1. **Use the `compat-date-at` tool** (no arguments) to get the full list of flags with their enable dates, categories, and annotations. This is faster and more accurate than manually reading the capnp file.
2. For each flag, also extract a one-line summary from the comment in `src/workerd/io/compatibility-date.capnp`.
3. Output a summary table grouped by category (streams, nodejs, containers, general, etc.):

   | Flag        | Enable date | Description       |
   | ----------- | ----------- | ----------------- |
   | `flag_name` | 2025-01-15  | Brief description |

   Include the total count of compatibility flags.

**If an argument is provided, look up that specific flag:**

1. **Use the `compat-date-at` tool** with `flag: "<argument>"` to get the flag's metadata (enable/disable names, enable date, annotations). Then read the capnp source for the comment.

2. **Extract flag metadata** from the tool output and capnp definition:
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
