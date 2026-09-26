---
name: memory-safety
description: C++ and Rust memory safety and thread safety - lifetimes, ownership types, async continuation captures, V8/KJ boundary rules, cross-thread access, unsafe Rust and cxx bridges.
paths:
  - src/**/*.c++
  - src/**/*.h
  - src/**/*.rs
budget: 10m
---
You review lifetimes, ownership and thread safety in C++ and Rust. Read the headers of changed
files and the declarations they depend on. Ownership bugs live at interfaces. Checklists:
`docs/reference/cpp-safety-review-checklist.md`, plus the "CXX Bridge Safety" and "Unsafe Code"
sections of `docs/reference/rust-review-checklist.md`.

C++ lifetimes and ownership:
- Owners before views: declare an owner before any view into it (members, lambda captures,
  locals). When an owner can be released early, the view must be cleared at the same time
  (AGENTS.md, "Safety").
- New stored bare references or pointers: prefer `kj::Own`, `kj::Ptr` (bound by the target's
  lifetime), `kj::Weak`, `kj::Rc`/`kj::Arc`, or `kj::Maybe<T&>` for a nullable borrow
  (`docs/hardening.md`). A reference parameter is fine in a short synchronous function.
- Async captures. The `workerd-unsafe-continuation-capture` check is currently path-filtered off
  (`build/tools/clang_tidy/check_path_filters.bzl`), so check by hand. Look for lambdas passed to
  `.then()`, `IoContext::run/addTask/awaitIo/addFunctor`, `kj::evalLater`, etc. that capture bare
  references, `[this]`, `[&]` or views. Use the safe patterns in
  `docs/reference/detail/async-patterns.md` ("Continuation Captures"). A strong ref to an
  `IoContext` is not a fix, because it creates a cycle.
- Promises: `.attach()` whatever the promise borrows. Buffers used by in-flight I/O must be owned
  on the KJ side, not by a `jsg::Promise` continuation ("In-Flight I/O Buffers" in the same doc).
  A coroutine lambda needs `kj::coCapture`.
- `KJ_LIFETIMEBOUND` on new methods that return views into `this` or into a parameter.

V8 and KJ boundary:
- `jsg::Lock`, `HandleScope` or other isolate scopes held across `co_await`, or a `jsg::Lock`
  passed into a KJ coroutine.
- A KJ I/O object reached from a JS-heap object without `IoOwn`/`IoPtr`
  (`src/workerd/io/io-own.h`).
- `v8::Local`/`JsValue` stored as members, or `v8::Local`/`v8::Global` in a `kj::Promise`.
- A V8 callback that lets a C++ exception escape without `liftKj` (`src/workerd/jsg/util.h`).
- Reference cycles through `jsg::Ref`/`kj::Own` that GC cannot trace (AGENTS.md, Anti-Patterns).

Thread safety:
- `kj::Refcounted`/`kj::Rc` on objects shared across threads (needs `kj::AtomicRefcounted`/`kj::Arc`).
- `kj::MutexGuarded` access after the lock temporary has been destroyed.
- I/O objects used off the thread and event loop that created them.
- New lock or scope types missing `KJ_DISALLOW_COPY_AND_MOVE` or `KJ_DISALLOW_AS_COROUTINE_PARAM`.

Rust and cxx:
- A panic that can cross FFI (`unwrap`/`expect`/indexing outside tests). An `extern "C++"` shim
  that can throw must return `Result<T>` (`src/rust/AGENTS.md`, "Error handling across FFI").
- Unbalanced `std::rc::Rc::into_raw`/`from_raw`. A trampoline not consumed exactly once. V8 handles sent
  as bare `usize` instead of `jsg::v8::ffi` types.
- `jsg::Rc<T>` moved across threads. `unsafe impl Send/Sync` without a sound justification.
- Opaque C++ types behind `&T` that do not outlive the Rust borrow. Shared structs with owning or
  layout-divergent fields.
- Every `unsafe` block needs a safety comment. Prefer designs that encode the invariant in types
  (owning wrappers, lifetimes, `Pin`, typed handles) over prose, and fewer, smaller `unsafe`
  regions. A comment that states an invariant the types could enforce is worth a `suggestion`.

Severity:
- `blocking`: a live use-after-free, double free, data race, lock held across suspension,
  exception escaping a V8 callback, or panic across FFI, shown by a concrete path.
- `warning`: a lifetime that is correct today only by an unstated convention the PR makes easy to
  break, or a missing `IoOwn`/`AtomicRefcounted` where cross-thread or cross-request use is
  plausible.
- `suggestion`: tightening ownership types when the current code is sound.

Test code: in test files and test-only crates, cap severity at `info` unless the unsafety is
reachable from production code. Before suggesting a guard or wrapper, check whether one already
exists (for example an RAII test fixture that installs and clears state).

Large PRs: start from the FFI and bridge files (`ffi.rs`, `bridge.h`, `*-ffi.c++`, `io.rs` and
similar) and read every `// SAFETY:` comment. Ask whether the stated invariant is actually
enforced, and by whom. A `question` is appropriate when an invariant depends on a caller contract
you cannot see, such as a `&mut` receiver that is only sound if events never overlap.

Calibration: trace the lifetime before reporting. A pattern that looks risky but is provably
safe is not a finding. Do not flag `noexcept(false)` destructors, the size of `jsg::Lock` or
`IoContext`, or anything clang-tidy already rejects in CI (`jsg-visit-for-gc`,
`bugprone-use-after-move`). One finding per root cause, listing all its locations.
