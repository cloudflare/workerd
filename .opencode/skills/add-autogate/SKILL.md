---
name: add-autogate
description: Step-by-step guide for adding a new autogate to workerd for gradual rollout of risky changes, including enum registration, string mapping, usage pattern, and testing.
---

## Adding an Autogate

Autogates enable gradual rollout of risky code changes independent of binary releases. Unlike compatibility flags (which are permanent, date-based behavioral changes), autogates are temporary gates that can be toggled on/off via internal tooling during rollout, then removed once the change is stable.

### When to use an autogate vs a compat flag

| Use an autogate when...                       | Use a compat flag when...                  |
| --------------------------------------------- | ------------------------------------------ |
| Rolling out a risky internal change gradually | Changing user-visible behavior permanently |
| You need a kill switch during rollout         | The change is tied to a compatibility date |
| The gate will be removed once stable          | Users need to opt in or out explicitly     |

An autogate may later become a compat flag once the rollout is complete and the behavior should become permanent.

### Step 1: Add the enum value

Edit `src/workerd/util/autogate.h`. Add a new entry to the `AutogateKey` enum **before `NumOfKeys`**:

```cpp
enum class AutogateKey {
  TEST_WORKERD,
  // ... existing gates ...
  // Brief description of what this gate controls.
  MY_NEW_FEATURE,
  NumOfKeys  // Reserved for iteration.
};
```

Naming convention: `SCREAMING_SNAKE_CASE` for the enum value.

### Step 2: Add the string mapping

Edit `src/workerd/util/autogate.c++`. Add a `case` to the `KJ_STRINGIFY` switch **before the `NumOfKeys` case**:

```cpp
kj::StringPtr KJ_STRINGIFY(AutogateKey key) {
  switch (key) {
    // ... existing cases ...
    case AutogateKey::MY_NEW_FEATURE:
      return "my-new-feature"_kj;
    case AutogateKey::NumOfKeys:
      KJ_FAIL_ASSERT("NumOfKeys should not be used in getName");
  }
}
```

Naming convention: `kebab-case` for the string name. This string is what appears in runtime configuration (prefixed with `workerd-autogate-`).

### Step 3: Guard your code

Use `Autogate::isEnabled()` to conditionally execute the new code path:

```cpp
#include <workerd/util/autogate.h>

// At the point where behavior should change:
if (util::Autogate::isEnabled(util::AutogateKey::MY_NEW_FEATURE)) {
  // New code path
} else {
  // Old code path (keep until gate is removed)
}
```

### Step 4: Test

Three ways to test autogated code:

**A. The `@all-autogates` test variant** (automatic):

Every `wd_test()` and `kj_test()` generates a `@all-autogates` variant that enables all gates. If your feature is tested by existing tests, they'll automatically run with the gate enabled:

```bash
just stream-test //src/workerd/api/tests:my-test@all-autogates
```

**B. Targeted C++ test setup**:

In a C++ test file, enable specific gates:

```cpp
#include <workerd/util/autogate.h>

// In test setup:
util::Autogate::initAutogateNamesForTest({"my-new-feature"_kj});

// In test teardown:
util::Autogate::deinitAutogate();
```

**C. Environment variable**:

Set `WORKERD_ALL_AUTOGATES=1` to enable all gates when no explicit config is provided.

### Step 5: Build and verify

```bash
just build
just stream-test //path/to:my-test@               # Old behavior (gate off)
just stream-test //path/to:my-test@all-autogates   # New behavior (gate on)
```

### Step 6: Remove the gate (after rollout)

Once the feature is stable and fully rolled out:

1. Remove the `AutogateKey` enum value from `autogate.h`
2. Remove the `case` from `KJ_STRINGIFY` in `autogate.c++`
3. Remove all `Autogate::isEnabled()` checks, keeping only the new code path
4. If the behavior should become a permanent compat flag, migrate it to `compatibility-date.capnp` instead (load the `add-compat-flag` skill for that process)

### Checklist

- [ ] Enum value added to `AutogateKey` in `autogate.h` (before `NumOfKeys`)
- [ ] Comment describes what the gate controls
- [ ] String mapping added to `KJ_STRINGIFY` in `autogate.c++`
- [ ] Code guarded with `Autogate::isEnabled()`
- [ ] Old code path preserved (for rollback)
- [ ] `@all-autogates` test variant passes
- [ ] Tests cover both gated and ungated paths

### Files touched

| File                            | What to do                              |
| ------------------------------- | --------------------------------------- |
| `src/workerd/util/autogate.h`   | Add enum value with comment             |
| `src/workerd/util/autogate.c++` | Add case to `KJ_STRINGIFY`              |
| Your feature file(s)            | Guard code with `Autogate::isEnabled()` |
