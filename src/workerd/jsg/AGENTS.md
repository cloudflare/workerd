# JSG (JavaScript Glue)

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
