---
description: Look up an autogate, or list all autogates if no argument given
subtask: true
---

Look up autogate: $ARGUMENTS

**If no argument is provided (empty or blank), list all autogates:**

1. Read the `AutogateKey` enum in `src/workerd/util/autogate.h`.
2. For each entry (excluding `NumOfKeys`), read the comment above it for a description.
3. Read the string mapping in `src/workerd/util/autogate.c++` to get the config string for each.
4. Output a summary table:

   | Autogate    | Config string  | Description                    |
   | ----------- | -------------- | ------------------------------ |
   | `ENUM_NAME` | `"string-key"` | Brief description from comment |

   Include the total count of active autogates.

**If an argument is provided, look up that specific autogate:**

1. **Find the enum entry.** Search `src/workerd/util/autogate.h` for the autogate name. The argument may be the enum name (e.g., `SOME_FEATURE`), the string key, or a partial match.

2. **Find the string mapping.** Search `src/workerd/util/autogate.c++` for the corresponding entry in the string-to-enum mapping. Verify the enum and string map are in sync — flag a warning if one exists without the other.

3. **Find usage sites.** Search for where the autogate is checked:

   ```
   grep -rn 'AutogateKey::<name>\|isAutoGateEnabled.*<name>' src/ --include='*.h' --include='*.c++'
   ```

4. **Find tests.** Search for the autogate name in test files:

   ```
   grep -rn '<name>' src/ --include='*.wd-test' --include='*-test.js' --include='*-test.ts' --include='*-test.c++'
   ```

   Also check if any tests use the `@all-autogates` variant to exercise this gate.

5. **Output:**
   - **Enum**: `AutogateKey::<name>` (file:line)
   - **String key**: the string used in configuration (file:line)
   - **In sync**: yes/no — whether enum and string map match
   - **Usage sites**: file:line list of where the autogate is checked
   - **Tests**: file:line list of tests that reference this autogate
   - **Notes**: Autogates are temporary and should be removed once the feature is fully rolled out. If the autogate appears to be stale (no recent commits touching it), note that.
   - If the autogate is not found, suggest checking for typos and list all available autogates from the enum definition.
