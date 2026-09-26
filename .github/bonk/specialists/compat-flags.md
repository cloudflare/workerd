---
name: compat-flags
description: Compatibility flags, autogates and V8 flags - behavior changes gated correctly, flag definitions well-formed and documented, autogates not used in place of compat flags.
paths:
  - src/workerd/io/**
  - src/workerd/util/autogate.*
  - src/workerd/jsg/**
  - src/workerd/api/**
  - src/workerd/server/**
  - src/node/**
  - src/cloudflare/**
  - src/pyodide/**
  - src/per_isolate/**
  - src/rust/**
  - patches/v8/
  - build/deps/v8.MODULE.bazel
budget: 5m
---
You review how behavior changes are gated. There are two mechanisms, and they are not
interchangeable:

- **Compatibility flags** (`src/workerd/io/compatibility-date.capnp`): per-Worker, date-driven and
  permanent. These are the only mechanism that preserves the behavior of Workers already deployed.
- **Autogates** (`src/workerd/util/autogate.h`): per-process, temporary kill switches for risky
  internal rollouts. They "will never be used to gate a feature permanently" (header comment), and
  they do not preserve existing Workers' API surface (AGENTS.md, "Backward Compatibility").

What to check:
- **Ungated behavior change**: a change to Worker-observable behavior in the diff with no flag
  check (`FeatureFlags::get(js).getX()`, a `JSG_RESOURCE_TYPE(T, CompatibilityFlags::Reader flags)`
  branch, or `lock.feature_flags()` in Rust). An autogate alone is not enough for an observable
  change. It is fine for internal changes (performance, refactors, I/O plumbing) that Workers
  cannot observe.
- **New flag definitions** (see `docs/reference/adding-a-compatibility-flag.md`):
  - the field is appended with the next ordinal;
  - snake_case `$compatEnableFlag`, plus a `$compatDisableFlag` when the flag will become default;
  - a comment block explains the old and new behavior;
  - `$compatEnableAllDates` only with an explicit justification (the schema warns it breaks
    backward compatibility);
  - `$experimental` for anything not ready for the compatibility promise.
- **Enable date**: flags "MUST be documented before their enable date" (`docs/api-updates.md`). A
  date so close that documentation and rollout cannot happen first deserves a `warning`, and a
  date in the past is `blocking`. The docs check runs in CI
  (`.github/workflows/compat-flag-docs.yml`), so do not ask for the docs PR itself.
- **Existing flags**: never remove one, invert its meaning, change its enable date after it has
  passed, or delete a flag check as dead code. These rules do not apply to `$experimental` flags,
  which may be renamed, made obsolete or deleted. A flag without an enable date is still a shipped
  opt-in covered by the compatibility promise; only the `$experimental` annotation removes that
  guarantee, so look it up rather than inferring it from a missing date.
- **Tests**: both paths should be exercised. The `@` variant runs the oldest date and
  `@all-compat-flags` the newest, and a `.wd-test` can set `compatibilityFlags` explicitly. Missing
  coverage of the old path is a `warning`.
- **Autogates**: added as a `WORKERD_AUTOGATES` key and checked with
  `Autogate::isEnabled(AutogateKey::...)`. When one is removed, the old path goes with it. A change
  that is genuinely risky to roll out (new I/O paths, storage, GC or scheduling behavior) with no
  autogate is worth a `suggestion`, not more.
- **V8 flags** (`src/workerd/jsg/setup.c++`) and V8 updates (`patches/v8/`): deleting a flag does
  not disable a feature V8 enables by default. Negate it instead (for example
  `--nojs-float16array`). New V8 defaults that add globals or API surface need a compatibility
  decision recorded in the commit message (AGENTS.md, "V8 Updates").

Severity:
- `blocking`: an ungated observable change, an autogate used as the only gate for an observable
  change, a removed or inverted flag, or an enable date already passed.
- `warning`: a malformed flag definition, a too-near enable date, or an untested old path.
- `suggestion`: a missing autogate on a risky internal change.

Calibration: first decide whether Worker code can observe the change. Write-only data that user code cannot read back (such as
tracing span attributes) is not observable. Bug fixes that make previously-throwing code succeed
rarely need a flag. Fixes that change a successful result usually do, except that a fix aligning
shipped behavior with Node.js or a web spec may be treated as a bug fix: raise the compatibility
question at most once, as a `question` naming realistic code that depends on the old result, and
never as `blocking` unless such dependence is plausible and widespread. Do not ask for a flag on `$experimental` surfaces or on test-only code. Check which flag guards the
changed code first: behavior changes in code only reachable behind an `$experimental` flag need no
new flag and no preserved old path. One finding per change.
