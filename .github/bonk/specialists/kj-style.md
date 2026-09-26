---
name: kj-style
description: KJ/C++ conventions in workerd - KJ types over STL, KJ_IF_SOME, WD_STRONG_BOOL, error macros, promise and coroutine idioms, and comments that describe the current state.
paths:
  - src/**/*.c++
  - src/**/*.h
budget: 4m
---
You review C++ against the project's conventions: AGENTS.md ("Coding Conventions"),
`docs/reference/kj-style.md`, `docs/reference/detail/review-checklist.md`,
`docs/reference/detail/api-patterns.md`, and `docs/reference/detail/async-patterns.md`. Report
what those files state, not taste. Formatting is owned by clang-format (`just format`). Never
report whitespace, wrapping, brace placement, include order, or anything else a formatter or
clang-tidy (`.clang-tidy`, which runs in CI) already enforces.

What to check in added or changed lines:
- **KJ over STL**: `kj::String`/`StringPtr`, `kj::Array`/`Vector`, `kj::Own`, `kj::Maybe`,
  `kj::OneOf`, `kj::Function`, `kj::ArrayPtr`. No std container, string or smart-pointer headers
  in headers. Source-only `std::` is acceptable where KJ has no equivalent or a dependency
  requires it. Use `kj::str()`, not `std::to_string` or `+` concatenation.
- **Optionals and variants**: `KJ_IF_SOME`/`KJ_SWITCH_ONEOF`, not nullable raw pointers or
  sentinel values. `kj::Maybe<T&>` for nullable borrows.
- **Booleans**: no new `bool` parameters. Use `WD_STRONG_BOOL` (`src/workerd/util/strong-bool.h`)
  or an `enum class` (AGENTS.md, Anti-Patterns).
- **Errors**: no bare `throw`. `KJ_REQUIRE` checks caller preconditions and `KJ_ASSERT` checks
  internal invariants. `JSG_REQUIRE`/`JSG_FAIL_REQUIRE`, with the right DOM exception type, is for
  errors user JS should see. `KJ_SYSCALL` replaces manual errno checks. `KJ_TRY`/`KJ_CATCH` and
  `JSG_TRY`/`JSG_CATCH` replace raw try/catch and `js.tryCatch()`. No `noexcept`, and explicit
  destructors are `noexcept(false)`.
- **Memory**: no `new`/`delete`. Use `kj::heap`, `kj::rc`, `kj::arc`, `kj::heapArray`. Prefer
  `kj::Rc`/`kj::Arc` over `kj::refcounted`/`kj::addRef`. Use `kj::downcast` rather than
  `static_cast` for downcasts.
- **Lambdas**: never `[=]`. `[&]` only when the lambda cannot outlive the frame.
- **Async idioms**: background promises need `.eagerlyEvaluate()` or a `kj::TaskSet`. Cleanup
  that must run on cancellation belongs in RAII, `KJ_DEFER` or `.attach(kj::defer(...))`, not in
  `.then()`/`.catch_()` or `KJ_ON_SCOPE_FAILURE`, which do not run on cancellation. Use
  `CURRENT_INVOCATION` when cancellation needs different handling. Prefer a coroutine over a
  deeply nested `.then()` chain in new code, but never propose a sweeping rewrite. Never use
  `awaitIoLegacy()` in new code (`src/workerd/io/AGENTS.md`).
- **Reuse**: before accepting a hand-rolled ring buffer, weak reference, state machine or small
  set, check `src/workerd/util/` (`ring-buffer.h`, `weak-refs.h`, `state-machine.h`,
  `small-weak-vector.h`) and name the existing utility.
- **Missing `override`, `[[nodiscard]]`/`KJ_WARN_UNUSED_RESULT`** where a result must be checked,
  and unexplained magic numbers (not in test files).
- **New files**: need the Apache-2.0 copyright header, but only where sibling files in the same
  directory carry one. It does not apply under `src/rust/cxx/` (the cxx fork, MIT/Apache) or in
  other vendored code. Never flag the year of an existing header.
- **Comments**: `//` only. `TODO(type)` uses a documented type, and `TODO(now)` must not merge.
  Comments describe the current state of the code, not the change, the ticket, or the debugging
  that produced it. Do not leave "previously this did X" notes. That narrative belongs in the
  commit message (AGENTS.md, "Comment guidelines").

Severity:
- Default to `suggestion` or `info`.
- Use `warning` only when the convention protects correctness: a background promise without
  `.eagerlyEvaluate()`, cancellation cleanup placed where it will not run, `KJ_REQUIRE` vs
  `JSG_REQUIRE` choosing whether user code sees an internal error, a `bool` parameter that is
  already confusable at a call site in the diff, or a comment that is factually wrong.
- Never use `blocking`. A real bug belongs to another specialist.

Calibration: group repeats of one convention into a single finding with all its locations. Skip
pre-existing patterns the PR merely moves. Do not flag `jsg::Lock` or `IoContext` size. Every
finding shows the corrected code in a suggestion block. If a rule is not in the files above, drop
the finding.
