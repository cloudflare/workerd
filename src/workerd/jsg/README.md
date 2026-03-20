# JSG Binding Layer Reference

Terse reference for the JSG (JavaScript Glue) C++/V8 binding layer. For narrative tutorial,
see [docs/jsg.md](../../../docs/jsg.md).

For file map and coding invariants, see [AGENTS.md](AGENTS.md).

## Primitive Type Mapping

| C++ Type         | v8 Type         | JavaScript Type | Notes                                      |
| ---------------- | --------------- | --------------- | ------------------------------------------ |
| `bool`           | `v8::Boolean`   | `boolean`       |                                            |
| `double`         | `v8::Number`    | `number`        |                                            |
| `int`            | `v8::Integer`   | `number`        |                                            |
| `int8_t`         | `v8::Integer`   | `number`        |                                            |
| `int16_t`        | `v8::Integer`   | `number`        |                                            |
| `int32_t`        | `v8::Int32`     | `number`        |                                            |
| `int64_t`        | `v8::BigInt`    | `bigint`        |                                            |
| `uint`           | `v8::Integer`   | `number`        |                                            |
| `uint8_t`        | `v8::Integer`   | `number`        |                                            |
| `uint16_t`       | `v8::Integer`   | `number`        |                                            |
| `uint32_t`       | `v8::Uint32`    | `number`        |                                            |
| `uint64_t`       | `v8::BigInt`    | `bigint`        |                                            |
| `kj::String`     | `v8::String`    | `string`        | Owned; JS `string` never → `kj::StringPtr` |
| `kj::StringPtr`  | `v8::String`    | `string`        | View; C++→JS only                          |
| `jsg::USVString` | `v8::String`    | `string`        | USV-encoded variant                        |
| `kj::Date`       | `v8::Date`      | `Date`          |                                            |
| `nullptr`        | `v8::Null`      | `null`          | Via `kj::Maybe<T>`                         |
| `nullptr`        | `v8::Undefined` | `undefined`     | Via `jsg::Optional<T>`                     |

## Nullable/Optional Semantics

| Type                      | JS `undefined` → C++ | JS `null` → C++ | C++ empty → JS |
| ------------------------- | -------------------- | --------------- | -------------- |
| `kj::Maybe<T>`            | `kj::none`           | `kj::none`      | `null`         |
| `jsg::Optional<T>`        | `kj::none`           | **throws**      | `undefined`    |
| `jsg::LenientOptional<T>` | `kj::none`           | `kj::none`      | `undefined`    |

## Composite Type Mapping

| C++ Type                      | JS Type                     | Notes                                                       |
| ----------------------------- | --------------------------- | ----------------------------------------------------------- |
| `kj::Array<T>`                | `Array`                     | Input must be JS array                                      |
| `kj::ArrayPtr<T>`             | `Array`                     | View                                                        |
| `kj::Array<kj::byte>`         | `ArrayBuffer`/`TypedArray`  | Shared backing store, no copy                               |
| `kj::HashSet<T>`              | `Set`                       | No custom struct keys                                       |
| `jsg::Sequence<T>`            | `Symbol.iterator` → `Array` | Accepts any iterable; outputs array                         |
| `jsg::Generator<T>`           | `Symbol.iterator`           | Synchronous per-item iteration                              |
| `jsg::AsyncGenerator<T>`      | `Symbol.asyncIterator`      | Async per-item iteration                                    |
| `jsg::Dict<T>`                | `Object`                    | Record type; string keys, uniform value type                |
| `kj::OneOf<T...>`             | Union                       | Web IDL validated at compile time                           |
| `jsg::Function<Ret(Args...)>` | `Function`                  | Bidirectional: JS↔C++ callable                             |
| `jsg::Promise<T>`             | `Promise`                   | Full `.then()`/`.catch_()` API                              |
| `jsg::Name`                   | `string` or `Symbol`        | Property name wrapper                                       |
| `jsg::BufferSource`           | `ArrayBuffer`/`TypedArray`  | Type-preserving; supports detach                            |
| `jsg::V8Ref<T>`               | Any V8 type                 | Persistent strong reference                                 |
| `jsg::Value`                  | Any                         | Alias for `V8Ref<v8::Value>`                                |
| `jsg::Ref<T>`                 | Resource wrapper            | Strong ref to JSG Resource Type                             |
| `jsg::HashableV8Ref<T>`       | Any V8 type                 | `V8Ref` + `hashCode()`                                      |
| `jsg::MemoizedIdentity<T>`    | Any                         | Preserves JS object identity across round-trips             |
| `jsg::Identified<T>`          | Any                         | Captures JS object identity + unwrapped value               |
| `jsg::NonCoercible<T>`        | Exact type                  | No automatic coercion; `T` = `kj::String`, `bool`, `double` |
| `jsg::LenientOptional<T>`     | Optional                    | Silently ignores type errors → `undefined`                  |

## JsValue Type Mapping

| JsValue Type        | V8 Equivalent         | Description                                                         |
| ------------------- | --------------------- | ------------------------------------------------------------------- |
| `JsValue`           | `v8::Value`           | Generic; holds any JS value                                         |
| `JsBoolean`         | `v8::Boolean`         | Boolean                                                             |
| `JsNumber`          | `v8::Number`          | Double-precision number                                             |
| `JsInt32`           | `v8::Int32`           | 32-bit signed integer                                               |
| `JsUint32`          | `v8::Uint32`          | 32-bit unsigned integer                                             |
| `JsBigInt`          | `v8::BigInt`          | Arbitrary precision integer                                         |
| `JsString`          | `v8::String`          | String                                                              |
| `JsSymbol`          | `v8::Symbol`          | Symbol                                                              |
| `JsObject`          | `v8::Object`          | Object                                                              |
| `JsArray`           | `v8::Array`           | Array                                                               |
| `JsMap`             | `v8::Map`             | Map collection                                                      |
| `JsSet`             | `v8::Set`             | Set collection                                                      |
| `JsFunction`        | `v8::Function`        | Function                                                            |
| `JsPromise`         | `v8::Promise`         | Promise (state inspection only; use `jsg::Promise<T>` for chaining) |
| `JsDate`            | `v8::Date`            | Date object                                                         |
| `JsRegExp`          | `v8::RegExp`          | Regular expression                                                  |
| `JsArrayBuffer`     | `v8::ArrayBuffer`     | ArrayBuffer                                                         |
| `JsArrayBufferView` | `v8::ArrayBufferView` | TypedArray or DataView                                              |
| `JsUint8Array`      | `v8::Uint8Array`      | Uint8Array                                                          |
| `JsProxy`           | `v8::Proxy`           | Proxy object                                                        |

Key rules: stack-only allocation (enforced in debug); implicit conversion to `v8::Local<T>`;
use `JsRef<T>` to persist beyond current scope.

## JSG Macro Catalog

### Resource Type Declaration

| Macro                         | Description                                               |
| ----------------------------- | --------------------------------------------------------- |
| `JSG_RESOURCE_TYPE(T)`        | Required. Declares resource type with JS binding block    |
| `JSG_RESOURCE_TYPE(T, flags)` | With `CompatibilityFlags::Reader` for conditional members |

### Methods

| Macro                                        | Context  | Description                                           |
| -------------------------------------------- | -------- | ----------------------------------------------------- |
| `JSG_METHOD(name)`                           | Instance | Expose method on prototype; auto-marshals args/return |
| `JSG_METHOD_NAMED(jsName, cppMethod)`        | Instance | Different JS name (e.g., `delete` → `delete_`)        |
| `JSG_STATIC_METHOD(name)`                    | Class    | Expose static method on constructor                   |
| `JSG_STATIC_METHOD_NAMED(jsName, cppMethod)` | Class    | Different JS name for static                          |
| `JSG_CALLABLE(name)`                         | Instance | Make object callable as function                      |

### Properties

| Macro                                               | Scope     | Writable         | Overridable by subclass |
| --------------------------------------------------- | --------- | ---------------- | ----------------------- |
| `JSG_READONLY_PROTOTYPE_PROPERTY(name, getter)`     | Prototype | No               | Yes                     |
| `JSG_PROTOTYPE_PROPERTY(name, getter, setter)`      | Prototype | Yes              | Yes                     |
| `JSG_READONLY_INSTANCE_PROPERTY(name, getter)`      | Instance  | No               | No                      |
| `JSG_INSTANCE_PROPERTY(name, getter, setter)`       | Instance  | Yes              | No                      |
| `JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getter)` | Instance  | No               | No                      |
| `JSG_LAZY_INSTANCE_PROPERTY(name, getter)`          | Instance  | Yes (after eval) | No                      |
| `JSG_STATIC_CONSTANT(name)`                         | Class     | No               | N/A                     |

### Type Relationships

| Macro                                | Description                                             |
| ------------------------------------ | ------------------------------------------------------- |
| `JSG_INHERIT(Base)`                  | Declare JS prototype inheritance                        |
| `JSG_INHERIT_INTRINSIC(v8Intrinsic)` | Inherit from V8 intrinsic (e.g., `v8::kErrorPrototype`) |
| `JSG_NESTED_TYPE(T)`                 | Expose nested type constructor (e.g., `Foo.Bar`)        |
| `JSG_NESTED_TYPE_NAMED(T, jsName)`   | Nested type with different JS name                      |

### Iterators

| Macro                                                        | Description                          |
| ------------------------------------------------------------ | ------------------------------------ |
| `JSG_ITERATOR(Name, method, ValueType, State, nextFn)`       | Define sync iterator type            |
| `JSG_ASYNC_ITERATOR(Name, method, ValueType, State, nextFn)` | Define async iterator type           |
| `JSG_ITERABLE(method)`                                       | Set `Symbol.iterator` to method      |
| `JSG_ASYNC_ITERABLE(method)`                                 | Set `Symbol.asyncIterator` to method |

### Structs

| Macro                             | Description                                           |
| --------------------------------- | ----------------------------------------------------- |
| `JSG_STRUCT(field1, field2, ...)` | Declare value-type mapping; only listed fields mapped |

### TypeScript Generation

| Macro                         | Context             | Description                         |
| ----------------------------- | ------------------- | ----------------------------------- |
| `JSG_TS_ROOT`                 | `JSG_RESOURCE_TYPE` | Mark as root for TS generation      |
| `JSG_TS_OVERRIDE(...)`        | `JSG_RESOURCE_TYPE` | Customize generated TS definition   |
| `JSG_TS_DEFINE(...)`          | `JSG_RESOURCE_TYPE` | Insert additional TS definitions    |
| `JSG_STRUCT_TS_ROOT`          | `struct`            | Struct variant of `JSG_TS_ROOT`     |
| `JSG_STRUCT_TS_OVERRIDE(...)` | `struct`            | Struct variant of `JSG_TS_OVERRIDE` |
| `JSG_STRUCT_TS_DEFINE(...)`   | `struct`            | Struct variant of `JSG_TS_DEFINE`   |

### Serialization

| Macro                               | Context                         | Description                                          |
| ----------------------------------- | ------------------------------- | ---------------------------------------------------- |
| `JSG_SERIALIZABLE(tag, ...oldTags)` | After `JSG_RESOURCE_TYPE` block | Enable structured clone; first tag = current version |
| `JSG_SERIALIZABLE_ONEWAY(tag)`      | After `JSG_RESOURCE_TYPE` block | Serialize only (deserialize via different type)      |

### Error Handling

| Macro                                      | Description                                   |
| ------------------------------------------ | --------------------------------------------- |
| `JSG_REQUIRE(cond, type, msg...)`          | Throw JS error if condition false             |
| `JSG_REQUIRE_NONNULL(maybe, type, msg...)` | Unwrap `kj::Maybe`; throw if none             |
| `JSG_FAIL_REQUIRE(type, msg...)`           | Unconditionally throw JS error                |
| `JSG_ASSERT(cond, type, msg...)`           | Like `KJ_ASSERT` but produces JSG-style error |

### Memory and Performance

| Macro                               | Description                                   |
| ----------------------------------- | --------------------------------------------- |
| `JSG_MEMORY_INFO(TypeName) { ... }` | Shorthand for heap snapshot tracking          |
| `JSG_ASSERT_FASTAPI(Class::method)` | Compile-time assert V8 Fast API compatibility |

## Property Type Decision Matrix

| Use case                                  | Macro                             | Why                                               |
| ----------------------------------------- | --------------------------------- | ------------------------------------------------- |
| Standard API property                     | `JSG_PROTOTYPE_PROPERTY`          | Overridable by subclasses; GC-friendly            |
| Read-only standard API                    | `JSG_READONLY_PROTOTYPE_PROPERTY` | Same benefits, no setter                          |
| Must shadow in subclass                   | `JSG_INSTANCE_PROPERTY`           | Own-property semantics prevent prototype override |
| New global without breaking existing code | `JSG_LAZY_INSTANCE_PROPERTY`      | Evaluated once; user can override value           |
| Constant on constructor                   | `JSG_STATIC_CONSTANT`             | Read-only class-level value                       |

**Default choice:** `JSG_PROTOTYPE_PROPERTY` (or readonly variant). Use instance properties
only with specific justification — they break GC optimization.

## GC-Visitable Types

All types that must be visited in `visitForGc()` if held as Resource Type members:

| Type                        | Notes                            |
| --------------------------- | -------------------------------- |
| `jsg::Ref<T>`               | Strong ref to resource type      |
| `jsg::V8Ref<T>`             | Persistent ref to V8 value       |
| `jsg::Value`                | Alias for `V8Ref<v8::Value>`     |
| `jsg::HashableV8Ref<T>`     | Hashable variant of `V8Ref`      |
| `jsg::JsRef<T>`             | Persistent ref to JsValue type   |
| `jsg::Optional<T>`          | When `T` is GC-visitable         |
| `jsg::LenientOptional<T>`   | When `T` is GC-visitable         |
| `jsg::Name`                 | Property name (string or symbol) |
| `jsg::Function<Sig>`        | Wrapped JS/C++ function          |
| `jsg::Promise<T>`           | JS promise wrapper               |
| `jsg::Promise<T>::Resolver` | Promise resolver                 |
| `jsg::BufferSource`         | Buffer with JS handle            |
| `jsg::Sequence<T>`          | Iterable sequence                |
| `jsg::Generator<T>`         | Sync generator                   |
| `jsg::AsyncGenerator<T>`    | Async generator                  |
| `kj::Maybe<T>`              | When `T` is GC-visitable         |

## Error Type Catalog

| JSG Error Name             | JS Exception Type                       | When to Use                                    |
| -------------------------- | --------------------------------------- | ---------------------------------------------- |
| `TypeError`                | `TypeError`                             | Wrong argument type, missing required argument |
| `Error`                    | `Error`                                 | Generic error                                  |
| `RangeError`               | `RangeError`                            | Value out of valid range                       |
| `DOMOperationError`        | `DOMException("OperationError")`        | Operation failed for operation-specific reason |
| `DOMDataError`             | `DOMException("DataError")`             | Provided data is invalid                       |
| `DOMInvalidStateError`     | `DOMException("InvalidStateError")`     | Object in wrong state for operation            |
| `DOMNotSupportedError`     | `DOMException("NotSupportedError")`     | Operation not supported                        |
| `DOMSyntaxError`           | `DOMException("SyntaxError")`           | Invalid syntax                                 |
| `DOMInvalidAccessError`    | `DOMException("InvalidAccessError")`    | Invalid access to object                       |
| `DOMNotFoundError`         | `DOMException("NotFoundError")`         | Requested item not found                       |
| `DOMAbortError`            | `DOMException("AbortError")`            | Operation was aborted                          |
| `DOMInvalidCharacterError` | `DOMException("InvalidCharacterError")` | Invalid character in string                    |
| `DOMQuotaExceededError`    | `DOMException("QuotaExceededError")`    | Storage quota exceeded                         |

## JSG_TS_OVERRIDE Rules

### Full Replacement

Override starts with `export `, `declare `, `type `, `abstract `, `class `, `interface `,
`enum `, `const `, `var `, or `function ` → replaces the entire generated definition.
`declare` added automatically if not present.

Special case: `type X = never` → deletes the definition entirely.

### Merge (all other overrides)

Override is converted to a class and merged with the generated definition:

1. **Prefix inference:**
   - Starts with `extends `, `implements `, `{` → prepend `class <Name> `
   - Starts with `<` → prepend `class <Name>`
   - Otherwise → prepend `class `

2. **Suffix:** If override doesn't end with `}`, append ` {}`

3. **Merge rules:**
   - Different name → rename type and update all references
   - Type parameters → copy to generated type
   - Heritage clauses → replace generated heritage
   - Members:
     - New members → inserted at end
     - Same-name member → replaces generated member
     - Member typed `never` → removes generated member without inserting

### Examples

| Override                               | Effect                                  |
| -------------------------------------- | --------------------------------------- |
| `KVNamespaceListOptions`               | Rename type                             |
| `{ json<T>(): Promise<T> }`            | Replace `json()` method, keep others    |
| `<R = any> { read(): ... }`            | Add type param, replace listed methods  |
| `{ actorState: never }`                | Remove `actorState` member              |
| `extends EventTarget<Map>`             | Replace heritage                        |
| `class Body { json<T>(): Promise<T> }` | Full replacement (starts with `class `) |
| `type X = never`                       | Delete definition                       |

Notes:

- Renaming happens after all overrides applied; use original C++ names in cross-references
- Roots visited before overrides; new types referenced by overrides may need `JSG_TS_ROOT`
- `JSG_TS_DEFINE` inserts additional definitions; `declare` added automatically; once per block

## Serialization Pattern

### Signatures

```cpp
void serialize(jsg::Lock& js, jsg::Serializer& serializer);
static jsg::Ref<T> deserialize(jsg::Lock& js, TagEnum tag, jsg::Deserializer& deser);
```

Both may take additional `TypeHandler<T>&` trailing parameters.

### Raw Methods

| Serializer                                   | Deserializer                                        |
| -------------------------------------------- | --------------------------------------------------- |
| `writeRawUint32(uint32_t)`                   | `readRawUint32() → uint32_t`                        |
| `writeRawUint64(uint64_t)`                   | `readRawUint64() → uint64_t`                        |
| `writeRawBytes(ArrayPtr<const byte>)`        | `readRawBytes(size) → ArrayPtr<const byte>`         |
| `writeLengthDelimited(ArrayPtr<const byte>)` | `readLengthDelimitedBytes() → ArrayPtr<const byte>` |
| `writeLengthDelimited(StringPtr)`            | `readLengthDelimitedString() → String`              |
| `write(js, JsValue)`                         | `readValue(js) → JsValue`                           |
| `transfer(js, JsArrayBuffer)`                | `getVersion() → uint32_t`                           |

### Rules

- `JSG_SERIALIZABLE` MUST appear AFTER `JSG_RESOURCE_TYPE` block
- Tag enum values MUST NOT change once data has been serialized
- First tag = current version; subsequent tags = accepted old versions
- `deserialize()` receives the tag for version dispatch

## Web IDL Union Validation Rules

`kj::OneOf<T...>` is validated at compile time against these rules:

1. At most one boolean type
2. At most one numeric type
3. At most one string type
4. At most one object type (excludes interface-like, callback, dictionary-like, sequence-like)
5. At most one callback function type
6. At most one dictionary-like type
7. At most one sequence-like type
8. At most one nullable (`kj::Maybe`) or dictionary type combined
9. No duplicate types
10. No `Optional<T>` types (use `kj::Maybe<T>`)

## Web IDL Type Categories

| Web IDL Category  | JSG Concept            | C++ Types                                          |
| ----------------- | ---------------------- | -------------------------------------------------- |
| Boolean           | `BooleanType`          | `bool`, `NonCoercible<bool>`                       |
| Numeric           | `NumericType`          | `int`, `double`, `uint32_t`, etc.                  |
| String            | `StringType`           | `kj::String`, `USVString`, `DOMString`, `JsString` |
| Object            | `ObjectType`           | `v8::Local<v8::Object>`, `v8::Global<v8::Object>`  |
| Symbol            | `SymbolType`           | (not yet implemented)                              |
| Interface-like    | `InterfaceLikeType`    | `JSG_RESOURCE` types, `BufferSource`               |
| Callback function | `CallbackFunctionType` | `kj::Function<T>`, `Constructor<T>`                |
| Dictionary-like   | `DictionaryLikeType`   | `JSG_STRUCT` types, `Dict<V, K>`                   |
| Sequence-like     | `SequenceLikeType`     | `kj::Array<T>`, `Sequence<T>`                      |

## Module System Overview

| Type       | Description                                     | Resolution priority                      |
| ---------- | ----------------------------------------------- | ---------------------------------------- |
| `BUNDLE`   | Worker bundle (user code)                       | From bundle: Bundle → Builtin → Fallback |
| `BUILTIN`  | Runtime modules (`node:*`, `cloudflare:*`)      | From builtin: Builtin → Internal         |
| `INTERNAL` | Only importable by builtins (`node-internal:*`) | From internal: Internal only             |

## Module Info Types

| Type                 | Description                  | Use case                  |
| -------------------- | ---------------------------- | ------------------------- |
| ESM                  | V8-compiled ES module        | Standard JS modules       |
| `CommonJsModuleInfo` | CJS with `require`/`exports` | Node.js compat            |
| `DataModuleInfo`     | Binary data as `ArrayBuffer` | Embedded binary resources |
| `TextModuleInfo`     | Text content as string       | Embedded text             |
| `WasmModuleInfo`     | WebAssembly module           | `.wasm` files             |
| `JsonModuleInfo`     | Parsed JSON data             | Config files              |
| `ObjectModuleInfo`   | JSG C++ object as module     | C++ API exposure          |
| `CapnpModuleInfo`    | Cap'n Proto schema module    | Schema files              |

## Two Module Implementations

| Aspect            | Original (`modules.h`)         | New (`modules-new.h`)             |
| ----------------- | ------------------------------ | --------------------------------- |
| Specifiers        | `kj::Path`                     | URL-based                         |
| Thread safety     | Single isolate                 | Cross-replica safe                |
| import.meta       | No                             | Yes (`url`, `main`, `resolve()`)  |
| Import attributes | No                             | Yes                               |
| Bundles           | Flat registry                  | Modular `ModuleBundle`            |
| Use when          | Simpler control, existing code | New code, URL specifiers, sharing |

## Context Embedder Data Slots

| Slot | Enum                       | Purpose                           |
| ---- | -------------------------- | --------------------------------- |
| 0    | `RESERVED`                 | **Never use** — reserved by V8    |
| 1    | `GLOBAL_WRAPPER`           | Pointer to global object wrapper  |
| 2    | `MODULE_REGISTRY`          | Pointer to module registry        |
| 3    | `EXTENDED_CONTEXT_WRAPPER` | Extended type wrapper for context |
| 4    | `VIRTUAL_FILE_SYSTEM`      | Virtual file system               |
| 5    | `RUST_REALM`               | Rust realm pointer                |

## Wrappable Lifecycle

```
1. C++ object created (no JS wrapper)
         |
2. Object passed to JavaScript
         |
3. attachWrapper() → JS wrapper created
         |
4. JS wrapper ←→ C++ object linked
         |
5. GC may collect wrapper if:
   - No JS references
   - No strong Ref<T>s
   - Wrapper unmodified
         |
6. Wrapper collected, C++ alive →
   new wrapper on next JS access
         |
7. C++ destroyed →
   detachWrapper(); JS wrapper = empty shell
```

Internal fields: slot 0 = `WORKERD_WRAPPABLE_TAG`, slot 1 = `Wrappable*` pointer.
Check: `jsg::Wrappable::isWorkerdApiObject(object)`.

## BackingStore Factory Methods

| Method                                             | Template Param  | Description                                       |
| -------------------------------------------------- | --------------- | ------------------------------------------------- |
| `BackingStore::from<T>(js, kj::Array<byte>)`       | TypedArray type | From owned array (may copy if outside V8 sandbox) |
| `BackingStore::alloc<T>(js, size)`                 | TypedArray type | Zero-initialized allocation inside sandbox        |
| `BackingStore::wrap<T>(data, size, disposer, ctx)` | TypedArray type | External data with custom cleanup                 |

Supported template params: `v8::ArrayBuffer`, `v8::Uint8Array`, `v8::Int8Array`,
`v8::Uint8ClampedArray`, `v8::Uint16Array`, `v8::Int16Array`, `v8::Uint32Array`,
`v8::Int32Array`, `v8::Float32Array`, `v8::Float64Array`, `v8::BigInt64Array`,
`v8::BigUint64Array`, `v8::DataView`.

## Observer Hooks

| Observer                    | Method                            | When Called                               |
| --------------------------- | --------------------------------- | ----------------------------------------- |
| `CompilationObserver`       | `onEsmCompilationStart`           | ESM module compilation begins             |
| `CompilationObserver`       | `onScriptCompilationStart`        | Non-ESM script compilation                |
| `CompilationObserver`       | `onWasmCompilationStart`          | WebAssembly compilation                   |
| `CompilationObserver`       | `onWasmCompilationFromCacheStart` | WASM from cached data                     |
| `CompilationObserver`       | `onJsonCompilationStart`          | JSON module parsing                       |
| `CompilationObserver`       | `onCompileCacheFound`             | Cached compilation data hit               |
| `CompilationObserver`       | `onCompileCacheRejected`          | Cached data rejected (version mismatch)   |
| `CompilationObserver`       | `onCompileCacheGenerated`         | New cache data generated                  |
| `CompilationObserver`       | `onCompileCacheGenerationFailed`  | Cache generation failed                   |
| `ResolveObserver`           | `onResolveModule`                 | Module resolution begins                  |
| `InternalExceptionObserver` | `reportInternalException`         | Internal exception for metrics            |
| `IsolateObserver`           | `onDynamicEval`                   | `eval()`, `new Function()`, etc. detected |

`onXxxStart` methods return `kj::Own<void>` destroyed on completion (RAII timing).
Compilation `Option`: `BUNDLE` (user code) or `BUILTIN` (runtime modules).
Resolve `Context`: `BUNDLE`, `BUILTIN`, `BUILTIN_ONLY`.
Resolve `Source`: `STATIC_IMPORT`, `DYNAMIC_IMPORT`, `REQUIRE`, `INTERNAL`.

## RTTI Type Mapping

| C++ Type                  | RTTI Kind   |
| ------------------------- | ----------- |
| `void`                    | `voidt`     |
| `bool`                    | `boolt`     |
| `int`, `double`, etc.     | `number`    |
| `kj::String`, `USVString` | `string`    |
| `kj::Array<T>`            | `array`     |
| `kj::Maybe<T>`            | `maybe`     |
| `kj::OneOf<T...>`         | `oneOf`     |
| `jsg::Promise<T>`         | `promise`   |
| `jsg::Dict<V, K>`         | `dict`      |
| `JSG_STRUCT` types        | `structure` |
| `JSG_RESOURCE` types      | `structure` |
| `jsg::Function<T>`        | `function`  |

## V8 Fast API Requirements

For a method to be Fast API compatible:

- **Return type:** `void`, `bool`, `int32_t`, `uint32_t`, `float`, or `double`
- **Parameter types:** Primitives above, `v8::Local<v8::Value>`, `v8::Local<v8::Object>`,
  or TypeWrapper-unwrappable types
- **Method:** Regular/const instance method, optionally with `jsg::Lock&` first param
- Any `JSG_METHOD(name)` auto-enables fast path if signature is compatible
- Use `JSG_ASSERT_FASTAPI(Class::method)` for compile-time verification

## CompileCache

Process-lifetime in-memory cache for V8 compilation data (built-in modules only).

- Singleton: `jsg::CompileCache::get()`
- Lookup: `cache.find(key)` → `kj::Maybe<CompileCache::Data>`
- Store: `cache.add(key, shared_ptr<CachedData>)`
- Thread-safe (mutex-guarded)
- Entries never removed or replaced
