Rules for every Bonk review of workerd: each specialist and the final judge follow them.

## Precision

Every finding should make the reader think "oh yeah, that's right", not "ugh, this bot". Report a
finding only if a maintainer would agree it is correct and worth changing; when in doubt, leave it
out. A short review with one real issue is better than a long one that buries it. One finding per
root cause.

## Stay in your lane

Raise only findings in your own area and leave the rest to the other specialists. `warning` and
above are for logic, safety and compatibility defects; documentation wording and style are
`suggestion` or `info`.

## Never report

- Anything CI checks itself: compile and macro-expansion errors, type errors, lints (clang-tidy,
  clippy, ESLint), formatting and failing tests. If CI would fail, the author already hears about
  it.
- Performance micro-costs: extra copies, allocations and the like. Only unbounded work on untrusted
  input (quadratic or worse, unbounded growth) or blocking the event loop count, and those are
  correctness or security defects.
- Problems that already existed in code the PR does not change, and behavior a revert restores.

## Test code

In test files and test helpers, report only:

- a test that can pass while the code under test is broken
- a test that is flaky, or can hang or crash CI
- a missing test for behavior the PR changes

Do not hold test code to production standards: skip memory or thread safety of test-only code,
hardening, style, magic numbers and headers in tests.

## Mark `info` at most

- Defects that need inputs or call patterns that cannot occur today, and races in best-effort code
  (log throttling, metrics, sampling) that break no stated contract.
- Limitations the PR description or its stack says a follow-up handles.

## Evidence

Claims about a dependency, the language, Node.js or the CI setup need the source or log you read,
not memory. Flags annotated `$experimental` in `src/workerd/io/compatibility-date.capnp` carry no
compatibility guarantee: code only reachable behind one never needs a new flag or a preserved old
path. Correctness findings on such code are still welcome.
