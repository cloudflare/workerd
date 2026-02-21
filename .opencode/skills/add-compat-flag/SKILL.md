---
name: add-compat-flag
description: Step-by-step guide for adding a new compatibility flag to workerd, including capnp schema, C++ usage, testing, and documentation requirements.
---

## Adding a Compatibility Flag

Compatibility flags control behavioral changes in workerd. They allow breaking changes to be rolled out gradually using compatibility dates. Follow these steps in order.

### Step 1: Choose flag names

Every flag needs:

- **Enable flag**: Opts in to the new behavior (e.g., `text_decoder_replace_surrogates`)
- **Disable flag**: Opts out after it becomes default (e.g., `disable_text_decoder_replace_surrogates`). Only needed if the flag will eventually become default for all workers.

Naming conventions:

- Use `snake_case`
- Enable flag describes the new behavior positively
- Disable flag uses a `no_` or `disable_` prefix, or describes the old behavior

### Step 2: Add to `compatibility-date.capnp`

Edit `src/workerd/io/compatibility-date.capnp`. Add a new field at the end of the `CompatibilityFlags` struct.

```capnp
  myNewBehavior @163 :Bool
      $compatEnableFlag("my_new_behavior")
      $compatDisableFlag("no_my_new_behavior")
      $compatEnableDate("2026-03-15");
  # Description of what this flag changes and why.
  # Include context about the old behavior and what the new behavior fixes.
```

Key points:

- The field number (`@163`) must be the next sequential number. Check the last field in the file.
- The field name is `camelCase` and becomes the C++ getter name (e.g., `getMyNewBehavior()`).
- `$compatEnableDate` is the date after which new workers get this behavior by default. Set this to a future date. If the flag is not yet ready for a default date, omit `$compatEnableDate` — the flag will only activate when explicitly listed in `compatibilityFlags`.
- Add `$experimental` annotation if the feature is experimental and should require `--experimental` to use.
- The comment block is required and serves as internal documentation.

Available annotations:
| Annotation | Purpose |
|---|---|
| `$compatEnableFlag("name")` | Flag name to enable the behavior |
| `$compatDisableFlag("name")` | Flag name to disable after it's default |
| `$compatEnableDate("YYYY-MM-DD")` | Date after which behavior is default |
| `$compatEnableAllDates` | Force-enable for all dates (rare, breaks back-compat) |
| `$experimental` | Requires `--experimental` flag to use |
| `$neededByFl` | Must be propagated to Cloudflare's FL proxy layer |
| `$impliedByAfterDate(name = "otherFlag", date = "YYYY-MM-DD")` | Implied by another flag after a date |

### Step 3: Use the flag in C++ code

Access the flag via the auto-generated getter:

```cpp
// In code that has access to jsg::Lock:
if (FeatureFlags::get(js).getMyNewBehavior()) {
  // New behavior
} else {
  // Old behavior
}
```

The `FeatureFlags` class is defined in `src/workerd/io/features.h`. The getter name is derived from the capnp field name with a `get` prefix and the first letter capitalized.

For JSG API classes, you can also access flags in `JSG_RESOURCE_TYPE`:

```cpp
JSG_RESOURCE_TYPE(MyApi, workerd::CompatibilityFlags::Reader flags) {
  if (flags.getMyNewBehavior()) {
    JSG_METHOD(newMethod);
  }
}
```

### Step 4: Add tests

Test both the old and new behavior. The test variant system helps:

- **`test-name@`** runs with the oldest compat date (2000-01-01) — tests old behavior
- **`test-name@all-compat-flags`** runs with the newest compat date (2999-12-31) — tests new behavior

In your `.wd-test` file, you can explicitly set the flag:

```capnp
const unitTests :Workerd.Config = (
  services = [(
    name = "my-test",
    worker = (
      modules = [(name = "worker", esModule = embed "my-test.js")],
      compatibilityFlags = ["my_new_behavior"],
    ),
  )],
);
```

For tests, the `compatibilityDate` field should not be included.

Write test cases that verify both behaviors. Consider edge cases where the flag changes observable behavior.

### Step 5: Document the flag

**This is required before the enable date.**

1. Create a PR in the [cloudflare-docs](https://github.com/cloudflare/cloudflare-docs) repository.
2. Add a markdown file under `src/content/compatibility-flags/` describing:
   - What the flag does
   - When it becomes default
   - How to opt in or opt out
   - Migration guidance if applicable

See `docs/api-updates.md` for more details on the documentation process.

### Step 6: Build and verify

```bash
# Build to verify the capnp schema compiles
just build

# Run the specific test
just stream-test //src/workerd/api/tests:my-test@

# Run with all compat flags to test the new behavior
just stream-test //src/workerd/api/tests:my-test@all-compat-flags

# Run the compatibility-date test to verify flag registration
just stream-test //src/workerd/io:compatibility-date-test@
```

### Checklist

- [ ] Flag added to `compatibility-date.capnp` with correct sequential field number
- [ ] Enable and disable flag names follow naming conventions
- [ ] Comment block describes old behavior, new behavior, and rationale
- [ ] Enable date is set (or intentionally omitted for experimental/unreleased flags)
- [ ] C++ code uses `FeatureFlags::get(js).getMyNewBehavior()` to branch on the flag
- [ ] Tests cover both old and new behavior
- [ ] Documentation PR created in cloudflare-docs (required before enable date)
- [ ] `compatibility-date-test` passes
