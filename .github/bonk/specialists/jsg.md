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
  - src/rust/api/**
budget: 6m
---
You review how C++ and Rust types are bound to JavaScript and traced by V8's garbage collector.
References: `src/workerd/jsg/AGENTS.md` ("INVARIANTS"), `src/workerd/jsg/README.md`
("GC-Visitable Types"), and `src/rust/AGENTS.md` ("JSG resources", "`Traced`").

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
- Use `JSG_TRY`/`JSG_CATCH` rather than the deprecated `js.tryCatch()`. `JSG_CATCH` cannot rethrow
  with `throw`.
- Module evaluation must hold a `Lock::ModuleEvaluationScope` and must not drain microtasks while
  nested (invariant 12).
- Rust: `&Lock`/`&mut Lock` is the first parameter after any `&self`/`&mut self` receiver.
  `#[jsg_method]` renames snake_case to camelCase, so check that the resulting JS name is the
  intended one.

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
