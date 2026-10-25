---
name: jsg-gc
description: JSG bindings and V8 GC safety - visitForGc coverage, JSG_STRUCT and member-type rules, ownership direction of Ref<T>, type conversions, allocation, and Rust JSG resources.
paths:
  - src/workerd/jsg/**
  - src/workerd/api/**/*.h
  - src/workerd/api/**/*.c++
  - src/workerd/io/**/*.h
  - src/workerd/io/**/*.c++
  - src/workerd/server/workerd-api.*
  - src/rust/jsg/**
  - src/rust/jsg-macros/**
  - src/rust/jsg-test/**
  - src/rust/api/**
  - src/rust/node-exceptions/**
  - src/rust/memory-cache/**
  - src/rust/**/*.c++
  - src/rust/**/*.h
budget: 6m
---
You review how C++ and Rust types are bound to JavaScript and traced by V8's garbage collector.
References: `src/workerd/jsg/AGENTS.md` ("INVARIANTS"), `src/workerd/jsg/README.md`
("GC-Visitable Types"), `src/rust/AGENTS.md` ("JSG resources", "`Traced`"), and
`src/rust/jsg/README.md` ("Safety rules") for Rust JSG.

GC tracing:
- A resource that holds `jsg::Ref`, `V8Ref`/`jsg::Value`, `JsRef`, `jsg::Function`,
  `jsg::Promise`, a `Resolver`, a generator, or `Maybe`/`Optional`/`Sequence` of those must visit
  every such field in `visitForGc()`. The `jsg-visit-for-gc` clang-tidy check runs in CI and
  catches direct member omissions, so spend your effort on what it cannot see:
  - references hidden inside lambdas (use `JSG_VISITABLE_LAMBDA`);
  - references in `kj::Own`ed helper objects or containers the check does not unwrap;
  - `// NOLINT(jsg-visit-for-gc)` added without a reason why skipping is safe.
- `Ref<T>` ownership flows owner to owned. Back-references use `T&`, `kj::Maybe<T&>` or
  `jsg::WeakRef<T>`. C++ reference cycles are never collected (AGENTS.md, Anti-Patterns).
- Rust: `jsg::Rc<T>`, `Option<jsg::Rc<T>>` and `Nullable<jsg::Rc<T>>` fields are traced
  automatically. `jsg::Weak<T>` is not. A `#[jsg_resource(custom_trace)]` impl must trace every strong field.

Rust JSG (rules and reasons in `src/rust/jsg/README.md` "Safety rules"; what to flag in
`docs/reference/rust-review-checklist.md` "Rust JSG Safety Rules"). The most common mistakes:
- An `std::rc::Rc` around a `TracedGlobal`, a `jsg::Rc`, or a type holding them, even if only one
  GC owner holds it so far. Once a clone reaches a second owner (for example a `Function::new`
  data object, or the other reaction function of a promise), a `jsg::Rc` aborts the process and a
  `TracedGlobal` can lose its value. The fix is a `jsg::Rc` or `jsg::Weak` to the resource that
  owns the state, which a method gets as `this: jsg::Rc<Self>`.
- A hand-written `impl Traced` whose `make_strong` skips a field that `trace` visits, or a
  `jsg::Rc` or `TracedGlobal` kept behind interior mutability other than `jsg::Cell` or
  `jsg::RefCell` (`UnsafeCell`, `OnceCell`). A reference moved out of a field by a plain Rust
  move is neither strong nor traced once GC has visited it there, so GC can collect the
  resource's wrapper, or the `TracedGlobal`'s value (#7976). The `jsg::` cells make it strong on
  the way out, and a `jsg::Rc` or `TracedGlobal` in a std `Cell` or `RefCell` field does not
  compile; what is left is what the compiler cannot see.
- A `jsg::v8::Global` in anything GC traces. It is always strong, so a cycle through it is never
  collected. It should be a `TracedGlobal`.
- A shim that calls `isolate->ThrowException` without checking `IsExecutionTerminating()` (the
  `isolate_throw_*` shims check), or a `jsg::Error` from a JavaScript call discarded with
  `.ok()`/`unwrap_or*`/`map_err`. Either one can let a terminated script keep running or replace
  a pending exception.
- New C++ JSG (`jsg::Lock`, `JSG_TRY`/`JSG_CATCH`, `jsg::check`, `createTunneledException`,
  `makeInternalError`, `jsg::Data`) in a C++ shim under `src/rust`. Shims use `v8::TryCatch` and
  `MaybeLocal` directly.

Member and struct types:
- No `v8::Local`/`JsValue` as class members, and no `v8::Local`/`v8::Global` in `JSG_STRUCT`
  fields. Use `jsg::V8Ref`/`jsg::JsRef`.
- `JSG_SERIALIZABLE` goes after the `JSG_RESOURCE_TYPE` block, and serialization tag values never
  change.
- Never unwrap `Ref<Object>`; use `V8Ref<v8::Object>`. Never opaque-wrap a `V8Ref<T>`.

Allocation and conversion:
- Allocate resources with `js.alloc<T>()`, or `js.allocAccounted` when the object owns large
  native memory. Outside `api/streams/`, use `JsReadableStream::create()`/`JsWritableStream::create()`
  rather than allocating `ReadableStream`/`WritableStream` (`src/workerd/api/AGENTS.md`).
- `recursivelyFreeze()` must never run on user-provided values, which may be cyclic.
- Prefer Web IDL-conformant parameter types. `NonCoercible<T>` runs counter to Web IDL and is
  avoided in new APIs. `jsg::Optional<T>` throws on `null`, `kj::Maybe<T>` accepts `null` and
  `undefined`, and `LenientOptional<T>` silently ignores type errors (README, "Nullable/Optional
  Semantics"; in Rust, `Option`/`Nullable`). Check which one the spec calls for.
- In C++ JSG code, use `JSG_TRY`/`JSG_CATCH` rather than the deprecated `js.tryCatch()`.
  `JSG_CATCH` cannot rethrow with `throw`. (Rust JSG's C++ shims use neither; see above.)
- Module evaluation must hold a `Lock::ModuleEvaluationScope` and must not drain microtasks while
  nested (invariant 12).
- Rust: `&Lock`/`&mut Lock` is the first parameter after any `&self`/`&mut self` receiver or
  `this: jsg::Rc<Self>` parameter. `#[jsg_method]` renames snake_case to camelCase, so check
  that the resulting JS name is the intended one.

Docs: when the change alters JSG macros, type mappings or invariants, `src/workerd/jsg/README.md`,
`docs/jsg.md` or `src/workerd/jsg/AGENTS.md` should change with it (the "CODE REVIEW RULE" in
that AGENTS.md). A missing update is `info`.

Severity:
- `blocking`: an untraced strong reference or an untraceable cycle that can cause a use-after-free
  or leak, a stored `v8::Local`, `recursivelyFreeze` on user input, or a changed serialization tag.
- `warning`: a wrong optional/nullable mapping versus the spec, `JSG_INSTANCE_PROPERTY` without a
  reason, or a `NOLINT` with no justification.
- `suggestion`: idiomatic JSG alternatives when the current code is correct.

Calibration: confirm that a field is really reachable from the JS heap before flagging it. Do not
duplicate what clang-tidy reports. One finding per root cause.
