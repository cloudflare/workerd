# Compatibility Date Migration Guide

Compatibility dates govern runtime behaviors and API availability in workerd. Setting a compatibility date ensures that existing workers run with consistent semantics over time, protecting applications from unexpected breaking changes.

## Overview

In workerd, versioning is driven by compatibility dates rather than traditional version numbers. As runtime features evolve or align with updated web specifications, behavior changes are gated behind specific dates.

A worker configured with a given date receives:
1. All compatibility flags enabled on or before that date.
2. The legacy behavior for any flags enabled after that date, unless opted in explicitly.

## Locating Your Current Compatibility Date

Compatibility dates are specified in configuration files depending on the environment:

* In `workerd.capnp` configuration files: Look for the `compatibilityDate` field within the worker definition:
  ```capnp
  compatibilityDate = "2024-01-01",
  ```
* In Wrangler configurations (`wrangler.toml` or `wrangler.jsonc`): Look for the `compatibility_date` field:
  ```toml
  compatibility_date = "2024-01-01"
  ```
* In `.wd-test` test definitions: Specified within the test worker structure. Note that test configurations typically omit the date to run against default test matrices.

## Planning a Date Upgrade

Upgrading a compatibility date should be done systematically:

### 1. Review Active Flags and Enable Dates
Every flag, its default enable date, and its behavior are defined in `src/workerd/io/compatibility-date.capnp`.
Consult this file or the Cloudflare compatibility flags documentation to understand changes introduced between your current date and your target date.

### 2. Incremental Adoption via Flags
Before advancing the global compatibility date for an application, individual flags can be tested in isolation by adding them to `compatibilityFlags`:
```capnp
compatibilityFlags = ["nodejs_compat_v2"],
```
This approach allows validating specific behavioral changes before committing to an updated date.

### 3. Opting Out of Specific Changes
If an updated compatibility date introduces a change that requires application updates, you can advance the date while temporarily opting out of that specific flag.
Flags support a disable prefix, typically `no_` or `disable_`:
```capnp
compatibilityDate = "2024-09-23",
compatibilityFlags = ["no_global_navigator"],
```
This enables all other improvements from the newer date while preserving previous behavior for the flagged feature during migration.

## Testing Compatibility Dates Locally

Always verify compatibility date changes locally prior to deployment:

### Local Runtime Verification
Execute `workerd` with your updated configuration:
```sh
workerd serve config.capnp
```

### Test Suite Execution
In repository test targets, the build system automatically generates multiple test variants:
* The `@` target runs against the baseline date (2000-01-01) with older behaviors.
* The `@all-compat-flags` target runs against future dates with all flags enabled.

Run test variants using the standard test command:
```sh
bazel test //src/workerd/api/tests:my-test@all-compat-flags
```
```sh
just stream-test //src/workerd/api/tests:my-test@all-compat-flags
```

## Related Documentation

* Adding a Compatibility Flag: `docs/reference/adding-a-compatibility-flag.md`
* Compatibility Flags Definition: `src/workerd/io/compatibility-date.capnp`
