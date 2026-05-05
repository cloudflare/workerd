# Code Review Checklist

When reviewing workerd C++ code, check for each of these items.

- **Always** check for STL leaking in: `std::string`, `std::vector`, `std::optional`, `std::unique_ptr`, etc.
- **Never** allow raw `new`/`delete`**. Should be `kj::heap<T>()` or similar
- **Never** use `throw` statements. Should use `KJ_ASSERT`/`KJ_REQUIRE`/`KJ_FAIL_ASSERT`/etc
- **Never** use `noexcept`
- **Always** use `noexcept(false)` on explicit destructors
- **Never** use `[=]` lambda captures
- **Never** use nullable raw pointers. Should be `kj::Maybe<T&>`
- **Never** mixed interface and implementation classes. No data members in interfaces, no non-final virtuals in implementations. Intermediate subclasses are ok
- **Never** use singletons or mutable globals
- **Always** verify proper use of `.attach()` on promises. Objects must stay alive for the promise duration
- **Always** use `.eagerlyEvaluate()` with background promises. Lazy continuations may never execute. Co-routines are eager by default.
- **Never** use manual errno checks. Should use `KJ_SYSCALL` / `KJ_SYSCALL_HANDLE_ERRORS`
- **Avoid** `static_cast` for downcasting where possible. Should be `kj::downcast<T>` (debug-checked)
- **Avoid** `dynamic_cast` for dispatch where possible. Extend the interface instead
- **Never** use `std::to_string` or `+` string concatenation**
- **Never** use `/* */` block comments. Use `//` line comments
- **Always** verify naming convention conformance. TitleCase types, camelCase functions/variables, CAPS constants
- **Always** check for missing braces around blocks. Required unless entire statement is on one line
- **Always** avoid `bool` function parameters. Prefer `enum class` or `WD_STRONG_BOOL` for clarity at call sites. E.g., `void connect(bool secure)` should be `void connect(SecureMode mode)`
- **Always** use `[[nodiscard]]` with functions returning error codes, `kj::Maybe`, or success booleans that callers must check should be `[[nodiscard]]`
- **Always** prefer coroutines over kj::Promise chains. Nested `.then()` chains with complex error handling that would be more readable as a coroutine with `co_await`. But **always** avoid suggesting sweeping rewrites
- **Always** check for missing `constexpr` / `consteval` where they would be appropriate
- **Always** avoid reinventing utility with custom code duplicating functionality already in `src/workerd/util/` (e.g., custom ring buffer, small set, state machine, weak reference pattern). **Always** check the util directory before suggesting a new abstraction.
- **Always** check for missing `override` on virtual method overrides.
- **Always** flag magic numbers (Numeric literals) without explanation or named constants.
- **Always** check for copyright header on new files. Every new `.c++` and `.h` file must begin with the project copyright/license header using the current year (not copied from older files). Expected format:
    ```
    // Copyright (c) <current-year> Cloudflare, Inc.
    // Licensed under the Apache 2.0 license found in the LICENSE file or at:
    //     https://opensource.org/licenses/Apache-2.0
    ```
    Flag any new file that uses a stale year (e.g., `2017-2022` in a file created in 2026) or omits the header entirely.
