# JSG (JavaScript Glue)

See `README.md` for terse reference (type mappings, macro catalog, error catalog).
See `docs/jsg.md` for narrative tutorial.

## OVERVIEW

Macro-driven C++/V8 binding layer: declares C++ types as JS-visible resources/structs with automatic type conversion.

## KEY FILES

| File             | Purpose                                                                                                                     |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `jsg.h`          | Core header (3115 lines): all macros (`JSG_RESOURCE_TYPE`, `JSG_METHOD`, etc.), type mappings, `Lock`, `V8Ref`, `GcVisitor` |
| `resource.h`     | Template metaprogramming: V8 callback generation, `FunctionCallbackInfo` dispatch, prototype/constructor wiring             |
| `struct.h`       | `JSG_STRUCT` value-type mapping: deep-copies C++ structs to/from JS objects                                                 |
| `wrappable.h`    | GC integration: `Wrappable` base class, CppGC visitor hooks, ref marking, weak pointers                                     |
| `promise.h`      | `jsg::Promise<T>` wrapping KJ promises ↔ JS promises; resolver pairs, coroutine integration                                |
| `modules.h`      | `ModuleRegistry`: ESM/CJS module resolution, evaluation, top-level await handling                                           |
| `modules-new.h`  | Replacement module system (new design)                                                                                      |
| `setup.h`        | `V8System`, `IsolateBase`, `JsgConfig`; process-level V8 init; `JSG_DECLARE_ISOLATE_TYPE`                                   |
| `function.h`     | `jsg::Function<Sig>` wrapping C++ callables ↔ JS functions                                                                 |
| `memory.h`       | `MemoryTracker`, `JSG_MEMORY_INFO` macro; heap snapshot support                                                             |
| `rtti.capnp`     | Cap'n Proto schema for type introspection; consumed by `types/` for TS generation                                           |
| `rtti.h`         | C++ RTTI builder: walks JSG type graph → `rtti.capnp` structures                                                            |
| `jsvalue.h`      | `JsValue`, `JsObject`, `JsString`, etc. — typed wrappers over `v8::Value`                                                   |
| `type-wrapper.h` | `TypeWrapper` template: compile-time dispatch for C++ ↔ V8 conversions                                                     |
| `meta.h`         | Argument unwrapping, `ArgumentContext`, parameter pack metaprogramming                                                      |
| `fast-api.h`     | V8 Fast API call optimizations                                                                                              |
| `ser.h`          | Structured clone: `Serializer`/`Deserializer`                                                                               |
| `web-idl.h`      | Web IDL types: `NonCoercible<T>`, `Sequence`, etc.                                                                          |
| `observer.h`     | `IsolateObserver`, `CompilationObserver` — hooks for metrics/tracing                                                        |

## BINDING PATTERN

```cpp
class MyType: public jsg::Object {
  static jsg::Ref<MyType> constructor(kj::String s);
  int getValue();
  void doThing(jsg::Lock& js, int n);

  JSG_RESOURCE_TYPE(MyType) {        // or (MyType, CompatibilityFlags::Reader flags)
    JSG_METHOD(doThing);
    JSG_PROTOTYPE_PROPERTY(value, getValue, setvalue);
    JSG_NESTED_TYPE(SubType);
    JSG_STATIC_METHOD(create);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(ref, v8ref);       // MUST trace all Ref<T>/V8Ref<T>/JsRef<T>
  }

  jsg::Ref<Other> ref;
  jsg::V8Ref<v8::Object> v8ref;
};
```

- Allocate resource objects: `js.alloc<MyType>(args...)`
- Value types: `JSG_STRUCT(field1, field2)` — auto-converted by value
- `jsg::Lock&` is the JS execution context; thread via method params
- `TypeHandler<T>&` as trailing param gives manual conversion access
- Compat flags param on `JSG_RESOURCE_TYPE` gates members conditionally

## ANTI-PATTERNS

- **NEVER** opaque-wrap `V8Ref<T>` — use handle directly
- **NEVER** use v8::Context embedder data slot 0 (`ContextPointerSlot::RESERVED`)
- **NEVER** hold traced handles (`Ref<T>`/`V8Ref<T>`) without marking in `visitForGc` — causes GC corruption
- **NEVER** use `FastOneByteString` in Fast API calls (GC corruption risk)
- **NEVER** unwrap `Ref<Object>` — use `V8Ref<v8::Object>` instead
- `JSG_CATCH` is NOT a true catch — cannot rethrow with `throw`
- `NonCoercible<T>` runs counter to Web IDL best practices; avoid in new APIs
- Rust JSG bindings: see `src/rust/jsg/` and `src/rust/jsg-macros/`

## INVARIANTS

These rules MUST be followed when writing or modifying JSG code:

1. **MUST implement `visitForGc()`** on any Resource Type holding `Ref<T>`, `V8Ref<T>`,
   `JsRef<T>`, `Function<T>`, `Promise<T>`, `Promise<T>::Resolver`, `BufferSource`, or
   `Name` — see `README.md` §GC-Visitable Types for the complete list
2. **MUST visit ALL GC-visitable fields** — missing one causes GC corruption
3. **MUST NOT store `v8::Local<T>` or `JsValue` types as class members** — use `V8Ref<T>`
   or `JsRef<T>` for persistence
4. **MUST NOT put `v8::Global<T>` or `v8::Local<T>` in `JSG_STRUCT` fields** — use
   `jsg::V8Ref<T>` or `jsg::JsRef<T>`
5. **MUST NOT put `v8::Global<T>` or `v8::Local<T>` in `kj::Promise`** — compile-time deleted
6. **MUST NOT pass `jsg::Lock` into KJ promise coroutines**
7. **`JSG_SERIALIZABLE` MUST appear AFTER `JSG_RESOURCE_TYPE` block**, not inside it
8. **Serialization tag enum values MUST NOT change** once data has been serialized
9. **`Ref<T>` ownership MUST flow owner→owned**; backwards refs use raw `T&` or
   `kj::Maybe<T&>`
10. **Prefer `JSG_PROTOTYPE_PROPERTY`** over `JSG_INSTANCE_PROPERTY` unless there's a
    specific reason — instance properties break GC optimization

## CODE REVIEW RULE

When reviewing changes to JSG code, check whether the change requires updates to
any of these documentation files:

- **`README.md`** — if the change adds/modifies type mappings, macros, error types,
  serialization patterns, or reference tables
- **`docs/jsg.md`** — if the change affects tutorial content, adds new patterns,
  or changes API usage examples
- **This file (`AGENTS.md`)** — if the change adds/removes files, changes the
  architecture summary, or introduces new invariants

Flag any needed doc updates in the review. Do not let behavioral or architectural
changes land without corresponding documentation updates.
