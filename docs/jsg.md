# JSG (JavaScript Glue) — Tutorial Guide

For terse reference tables (type mappings, macro catalog, error catalog), see
[`src/workerd/jsg/README.md`](../src/workerd/jsg/README.md).

For file map and coding invariants, see
[`src/workerd/jsg/AGENTS.md`](../src/workerd/jsg/AGENTS.md).

---

## 1. Introduction

`jsg` is an abstraction API used to hide many of the complexities of translating back and
forth between JavaScript and C++ types. Think of it as a bridge layer between the kj-based
internals of the runtime and the v8-based JavaScript layer.

Ideally, JSG would be a complete wrapper around V8, such that code using JSG does not need to
interact with V8 APIs at all. Perhaps, then, different implementations of JSG could provide
the same interface on top of different JavaScript engines. However, as of today, this is not
quite the case. At present application code will still need to use V8 APIs directly in some
cases. We would like to improve this in the future.

### Prerequisites

Before working with JSG, read both the ["KJ Style Guide"][] and ["KJ Tour"][] documents.
In particular, the ["KJ Style Guide"][] introduces a differentiation between "Value Types"
and "Resource Types":

> There are two kinds of types: values and resources. Value types are simple data
> structures; they serve no purpose except to represent pure data. Resource types
> represent live objects with state and behavior, and often represent resources
> external to the program.

JSG embraces these concepts and offers a C++-to-JavaScript type-mapping layer that is
explicitly built around them.

---

## 2. Getting Started

### `jsg::Lock&`

In order to execute JavaScript on the current thread, a lock must be acquired on the
`v8::Isolate`. The `jsg::Lock&` represents the current lock. It is passed as an argument to
many methods that require access to the JavaScript isolate and context. By convention, this
argument is always named `js`.

The `jsg::Lock` interface itself provides access to basic JavaScript functionality, such as
the ability to construct basic JavaScript values and call JavaScript functions.

For Resource Types, all methods declared with `JSG_METHOD` and similar macros optionally take
a `jsg::Lock&` as the first parameter:

```cpp
class Foo: public jsg::Object {
public:
  void foo(jsg::Lock& js, int x, int y) {
    // ...
  }

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(foo);
  }
};
```

Refer to the `jsg.h` header file for more detail on the specific methods available on
`jsg::Lock`.

### V8 System Setup

Before using any JSG functionality, you must initialize V8 by creating a `V8System` instance.
This performs process-wide initialization and should be created once, typically in `main()`.

```cpp
#include <workerd/jsg/setup.h>

int main() {
  // Basic initialization with default platform
  jsg::V8System v8System;

  // Or with V8 flags
  kj::ArrayPtr<const kj::StringPtr> flags = {"--expose-gc"_kj, "--single-threaded-gc"_kj};
  jsg::V8System v8System(flags);

  // Or with a custom platform
  auto platform = jsg::defaultPlatform(4);  // 4 background threads
  jsg::V8System v8System(*platform, flags, platform.get());

  // ... use JSG APIs ...
}
```

**Important:** Only one `V8System` can exist per process. It must be created before any other
JSG operations and destroyed last.

#### Custom Platform

The default V8 platform uses a background thread pool for tasks like garbage collection and
compilation. You can customize the thread pool size:

```cpp
// Create platform with specific thread count
// Pass 0 for auto-detect (uses available CPU cores)
kj::Own<v8::Platform> platform = jsg::defaultPlatform(0);
```

In production (like workerd), you may want to wrap the platform to customize behavior.
For example, workerd wraps the platform to control `Date.now()` timing:

```cpp
// In workerd/server/v8-platform-impl.h
class WorkerdPlatform final: public v8::Platform {
public:
  explicit WorkerdPlatform(v8::Platform& inner): inner(inner) {}

  // Override to return KJ time instead of system time
  double CurrentClockTimeMillis() noexcept override;

  // All other methods delegate to inner platform
  // ...
private:
  v8::Platform& inner;
};

// Usage in main():
auto platform = jsg::defaultPlatform(0);
WorkerdPlatform v8Platform(*platform);
jsg::V8System v8System(v8Platform, flags, platform.get());
```

#### Fatal Error Handling

You can set a custom callback for fatal V8 errors:

```cpp
void myFatalErrorHandler(kj::StringPtr location, kj::StringPtr message) {
  KJ_LOG(FATAL, "V8 fatal error", location, message);
}

jsg::V8System::setFatalErrorCallback(&myFatalErrorHandler);
```

### `Isolate<TypeWrapper>`

An Isolate represents an independent V8 execution environment. Multiple isolates can run
concurrently on different threads. Each isolate has its own heap and cannot directly share
JavaScript objects with other isolates.

To create an isolate, first declare your isolate type using `JSG_DECLARE_ISOLATE_TYPE`:

```cpp
// Declare an isolate type that can use MyGlobalObject and MyApiClass
JSG_DECLARE_ISOLATE_TYPE(MyIsolate, MyGlobalObject, MyApiClass, AnotherClass);
```

Then instantiate it:

```cpp
// Create an isolate observer (for metrics/debugging)
auto observer = kj::heap<jsg::IsolateObserver>();

// Create the isolate
MyIsolate isolate(v8System, kj::mv(observer));
```

#### Isolate with Configuration

If your API types require configuration (e.g., compatibility flags):

```cpp
struct MyConfiguration {
  bool enableFeatureX;
  kj::StringPtr version;
};

MyIsolate isolate(v8System, MyConfiguration{.enableFeatureX = true, .version = "1.0"_kj},
                  kj::mv(observer));
```

#### Isolate Groups (V8 Sandboxing)

When V8 sandboxing is enabled, isolates can be grouped to share a sandbox or isolated for
stronger security boundaries:

```cpp
// Use the default group (shared sandbox - more memory efficient)
// This is what workerd uses in production
auto group = v8::IsolateGroup::GetDefault();
MyIsolate isolate(v8System, group, config, kj::mv(observer));

// Or create a new isolate group for stronger isolation
// (separate sandbox, uses more memory)
auto isolatedGroup = v8::IsolateGroup::Create();
MyIsolate isolate(v8System, isolatedGroup, config, kj::mv(observer));
```

In workerd, the default group is used for all worker isolates. This allows multiple
isolates to share the V8 sandbox memory region, reducing overall memory usage while
still maintaining isolate-level JavaScript isolation.

### Taking a Lock

Before executing JavaScript, you must acquire a lock on the isolate. Only one thread can
hold the lock at a time:

```cpp
// Method 1: Using runInLockScope (recommended)
isolate.runInLockScope([&](MyIsolate::Lock& lock) {
  // JavaScript execution happens here
  auto context = lock.newContext<MyGlobalObject>();
  // ...
});

// Method 2: Manual lock (when you need more control)
jsg::runInV8Stack([&](jsg::V8StackScope& stackScope) {
  MyIsolate::Lock lock(isolate, stackScope);
  lock.withinHandleScope([&] {
    // ...
  });
});
```

### Creating a Context

A context provides the global object and scope for JavaScript execution:

```cpp
isolate.runInLockScope([&](MyIsolate::Lock& lock) {
  // Create a context with MyGlobalObject as the global
  jsg::JsContext<MyGlobalObject> jsContext = lock.newContext<MyGlobalObject>();

  // Access the context handle for V8 operations
  v8::Local<v8::Context> v8Context = jsContext.getHandle(lock);

  // Enter the context to execute JavaScript
  v8::Context::Scope contextScope(v8Context);

  // Now you can execute JavaScript...
});
```

#### Context Options

```cpp
jsg::NewContextOptions options {
  // Add options as needed
};
auto context = lock.newContext<MyGlobalObject>(options, constructorArg1, constructorArg2);
```

### Using the Lock

The `Lock` class provides access to type wrapping and unwrapping:

```cpp
isolate.runInLockScope([&](MyIsolate::Lock& lock) {
  auto jsContext = lock.newContext<MyGlobalObject>();
  v8::Local<v8::Context> context = jsContext.getHandle(lock);
  v8::Context::Scope contextScope(context);

  // Wrap a C++ value to JavaScript
  v8::Local<v8::Value> jsValue = lock.wrap(context, kj::str("hello"));

  // Unwrap a JavaScript value to C++
  kj::String cppValue = lock.unwrap<kj::String>(context, jsValue);

  // Get a type handler for specific operations
  const auto& handler = lock.getTypeHandler<MyApiClass>();

  // Get a constructor function
  jsg::JsObject constructor = lock.getConstructor<MyApiClass>(context);
});
```

### `IsolateBase`

`IsolateBase` is the non-templated base class of `Isolate<T>`, providing common functionality:

```cpp
// Get the IsolateBase from a v8::Isolate pointer
jsg::IsolateBase& base = jsg::IsolateBase::from(v8Isolate);

// Terminate JavaScript execution (can be called from another thread)
base.terminateExecution();

// Configure isolate behavior
base.setAllowEval(lock, true);           // Enable/disable eval()
base.setCaptureThrowsAsRejections(lock, true);  // Convert throws to rejections
base.setNodeJsCompatEnabled(lock, true);  // Enable Node.js compatibility

// Check configuration
bool nodeCompat = base.isNodeJsCompatEnabled();
bool topLevelAwait = base.isTopLevelAwaitEnabled();
```

### Complete Example

```cpp
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>

// Define your API types
class MyGlobalObject: public jsg::Object, public jsg::ContextGlobal {
public:
  kj::String greet(kj::String name) {
    return kj::str("Hello, ", name, "!");
  }

  JSG_RESOURCE_TYPE(MyGlobalObject) {
    JSG_METHOD(greet);
  }
};

// Declare the isolate type
JSG_DECLARE_ISOLATE_TYPE(MyIsolate, MyGlobalObject);

int main() {
  // Initialize V8
  jsg::V8System v8System;

  // Create isolate
  auto observer = kj::heap<jsg::IsolateObserver>();
  MyIsolate isolate(v8System, kj::mv(observer));

  // Execute JavaScript
  isolate.runInLockScope([&](MyIsolate::Lock& lock) {
    auto jsContext = lock.newContext<MyGlobalObject>();
    v8::Local<v8::Context> context = jsContext.getHandle(lock);
    v8::Context::Scope contextScope(context);

    // Compile and run JavaScript
    auto source = lock.str("greet('World')");
    auto script = jsg::check(v8::Script::Compile(context, source));
    auto result = jsg::check(script->Run(context));

    // Convert result to C++ string
    kj::String greeting = lock.unwrap<kj::String>(context, result);
    KJ_LOG(INFO, greeting);  // "Hello, World!"
  });

  return 0;
}
```

### Real-World Reference: workerd Initialization

For a production example, see how workerd initializes V8 in `src/workerd/server/workerd.c++`:

```cpp
// From workerd.c++ serveImpl()
auto platform = jsg::defaultPlatform(0);
WorkerdPlatform v8Platform(*platform);
jsg::V8System v8System(v8Platform,
    KJ_MAP(flag, config.getV8Flags()) -> kj::StringPtr { return flag; },
    platform.get());
```

And how isolates are created in `src/workerd/server/server.c++`:

```cpp
// From server.c++ when creating a worker
auto isolateGroup = v8::IsolateGroup::GetDefault();
auto api = kj::heap<WorkerdApi>(globalContext->v8System, def.featureFlags, extensions,
    limitEnforcer->getCreateParams(), isolateGroup, kj::mv(jsgobserver),
    *memoryCacheProvider, pythonConfig);
```

---

## 3. Value Types

Value Types in JSG include both primitives (e.g. strings, numbers, booleans, etc) and
relatively simple structured types that do nothing but convey data. See the README for the
complete [primitive type mapping table](../src/workerd/jsg/README.md#primitive-type-mapping).

### How Does JSG Convert Values?

V8 provides an API for working with JavaScript types from within C++. Unfortunately, this API
can be a bit cumbersome to work with directly. JSG provides an additional set of mechanisms
that wrap the V8 APIs and translate those into more ergonomic C++ types and structures.
Exactly how this mechanism works is covered in the advanced section on
[Wrappable Internals](#19-wrappable-internals).

For example, when mapping from JavaScript into C++, when JSG encounters a string value, it
can convert that into either a `kj::String` or `jsg::USVString`, depending on what is needed
by the C++ layer. Likewise, when translating from C++ to JavaScript, JSG will generate a
JavaScript `string` whenever it encounters a `kj::String`, `kj::StringPtr`, or
`jsg::USVString`.

JSG will _not_ translate JavaScript `string` to `kj::StringPtr`.

### Nullable/Optional Types (`kj::Maybe<T>` and `jsg::Optional<T>`)

A nullable type `T` can be either `null` or `T`, where `T` can be any Value or Resource type.
This is represented in JSG using `kj::Maybe<T>`.

An optional type `T` can be either `undefined` or `T`. This is represented in JSG using
`jsg::Optional<T>`.

Take careful note of the differences: `kj::Maybe<kj::String>` allows `null` or `undefined`;
`jsg::Optional<kj::String>` only allows `undefined`.

At the C++ layer, `kj::Maybe<T>` and `jsg::Optional<T>` are semantically equivalent. Their
value is one of either `kj::none` or type `T`. When an empty `kj::Maybe<T>` is passed out to
JavaScript, it is mapped to `null`. When an empty `jsg::Optional<T>` is passed out to
JavaScript, it is mapped to `undefined`.

This means that JavaScript `undefined` can be translated to `kj::Maybe<T>`, but
`kj::Maybe<T>` passed back out to JavaScript will translate to JavaScript `null`.

See the README for the [nullable/optional semantics table](../src/workerd/jsg/README.md#nullableoptional-semantics).

### Union Types (`kj::OneOf<T...>`)

A union type `<T1, T2, ...>` is one whose value is one of the types listed. It is represented
in JSG using `kj::OneOf<T...>`.

For example, `kj::OneOf<kj::String, uint16_t, kj::Maybe<bool>>` can be either: a) a string,
b) a 16-bit integer, c) `null`, or (d) `true` or `false`.

**Important:** JSG validates union types at compile-time according to Web IDL rules:

- A union can have at most one nullable type (`kj::Maybe<T>`) or dictionary type
- A union can have at most one numeric type, one string type, one boolean type, etc.
- Multiple interface types (JSG_RESOURCE types) are allowed as long as they are distinct

For example:

```cpp
// Valid: different distinguishable categories
kj::OneOf<kj::String, double, bool>  // string, numeric, boolean

// Valid: multiple interface types (different classes)
kj::OneOf<Ref<ClassA>, Ref<ClassB>>

// Invalid: multiple numerics in same union
kj::OneOf<int, double>  // Compile error!

// Invalid: multiple strings in same union
kj::OneOf<kj::String, USVString>  // Compile error!

// Invalid: dictionary + nullable
kj::OneOf<kj::Maybe<int>, MyStruct>  // Compile error!
```

See the README's [Web IDL union validation rules](../src/workerd/jsg/README.md#web-idl-union-validation-rules)
for the complete list.

When a value could match multiple types in a union, JSG attempts to match types in a specific
order based on their Web IDL category. Interface types are checked first, then dictionaries,
then sequences, then primitives.

#### Web IDL Type Categories

Web IDL defines nine distinguishable type categories. JSG maps these to C++ concepts
(defined in `web-idl.h`):

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

You can use these concepts in your own code for type introspection:

```cpp
#include <workerd/jsg/web-idl.h>

template <typename T>
void process(T value) {
  if constexpr (jsg::webidl::StringType<T>) {
    // Handle string types
  } else if constexpr (jsg::webidl::NumericType<T>) {
    // Handle numeric types
  } else if constexpr (jsg::webidl::NonCallbackInterfaceType<T>) {
    // Handle JSG_RESOURCE types
  }
}
```

#### Nullable Type Counting

The `nullableTypeCount<T...>` template recursively counts nullable types in a union,
including through nested `kj::OneOf` types:

```cpp
// Count = 1
nullableTypeCount<kj::Maybe<int>>

// Count = 2 (nested nullables)
nullableTypeCount<kj::Maybe<kj::OneOf<kj::Maybe<int>, kj::String>>>
```

### Array Types (`kj::Array<T>` and `kj::ArrayPtr<T>`)

The `kj::Array<T>` and `kj::ArrayPtr<T>` types map to JavaScript arrays. Here, `T` can be
any value or resource type. The types `kj::Array<kj::byte>` and `kj::ArrayPtr<kj::byte>` are
handled differently (see [TypedArrays](#typedarrays)).

```cpp
void doSomething(kj::Array<kj::String> strings) {
  KJ_DBG(strings[0]);  // a
  KJ_DBG(strings[1]);  // b
  KJ_DBG(strings[2]);  // c
}
```

```js
doSomething(['a', 'b', 'c']);
```

### Set Type (`kj::HashSet<T>`)

The `kj::HashSet<T>` type maps to JavaScript sets. There are currently some restrictions —
for example, you cannot have a `kj::HashSet<CustomStruct>`.

```cpp
void doSomething(kj::HashSet<kj::String> strings) {
  KJ_DBG(strings.has("a"));
}
```

```js
doSomething(new Set(['a', 'b', 'c']));
```

### Sequence Types (`jsg::Sequence<T>`)

A [Sequence][] is any JavaScript object that implements `Symbol.iterator`. At the C++ level,
a `jsg::Sequence<T>` is semantically identical to `kj::Array<T>` but JSG handles the mapping
differently:

- With `kj::Array<T>`, the input from JavaScript _must_ always be a JavaScript array.
- With `jsg::Sequence<T>`, the input can be any object implementing `Symbol.iterator`.

When a `jsg::Sequence<T>` is passed back out to JavaScript, JSG always produces an array.

```cpp
void doSomething(jsg::Sequence<kj::String> strings) {
  KJ_DBG(strings[0]);  // a
}
```

```js
doSomething({
  *[Symbol.iterator]() {
    yield 'a';
    yield 'b';
    yield 'c';
  },
});

// or simply:
doSomething(['a', 'b', 'c']);
```

### Generator Types (`jsg::Generator<T>` and `jsg::AsyncGenerator<T>`)

`jsg::Generator<T>` provides an alternative to `jsg::Sequence` for handling objects that
implement `Symbol.iterator`. Whereas `jsg::Sequence` produces a complete copy of the iterable
sequence equivalent to `kj::Array`, `jsg::Generator<T>` provides an API for iterating over
each item one at a time.

```cpp
void doSomething(jsg::Generator<kj::String> strings) {
  strings.forEach([](jsg::Lock& js, kj::String value, jsg::GeneratorContext<kj::String> ctx) {
    KJ_DBG(value);
  });
}
```

For `jsg::Generator<T>`, iteration is fully synchronous. To support asynchronous generators
and async iteration, JSG provides `jsg::AsyncGenerator<T>` with a nearly identical C++ API.

### Record/Dictionary Types (`jsg::Dict<T>`)

A [Record][] type is an ordered set of key-value pairs. In JavaScript they are ordinary
objects whose string-keys all map to the same type of value.

```cpp
// Given jsg::Dict<bool>, a matching JS object would be:
// { "abc": true, "xyz": false, "foo": true }
```

Importantly, the keys are _always_ strings and the values are _always_ the same type.

### Non-Coercible and Lenient Types

By default, JSG supports automatic coercion between JavaScript types where possible. For
instance, when a `kj::String` is expected, any JavaScript value that can be coerced into a
string can be passed (including `null` → `'null'`, `undefined` → `'undefined'`).

`jsg::NonCoercible<T>` declares that you want a type `T` but do _not_ want automatic
coercion. For example, `jsg::NonCoercible<kj::String>` will _only_ accept string values.
At the time of this writing, the only supported values of `T` are `kj::String`, `bool`, and
`double`.

`jsg::LenientOptional<T>` is like `jsg::Optional<T>` but instead of throwing a type error
for incorrect values, it ignores them and passes `undefined` instead.

### TypedArrays (`kj::Array<kj::byte>` and `jsg::BufferSource`)

In V8, `TypedArray`s and `ArrayBuffer`s are backed by a `v8::BackingStore` instance. JSG
provides two type mappings.

**`kj::Array<kj::byte>`** — When receiving from JavaScript, the `kj::Array<kj::byte>`
provides a _view_ over the same underlying `v8::BackingStore` (no copy):

```
                   +------------------+
                   | v8::BackingStore |
                   +------------------+
                     /             \
   +-----------------+             +---------------------+
   | v8::ArrayBuffer | ----------> | kj::Array<kj::byte> |
   +-----------------+             +---------------------+
```

Because both are _mutable_, changes in one are immediately readable by the other. When a
`kj::Array<kj::byte>` is passed _out_ to JavaScript, it maps to a _new_ `ArrayBuffer`
over the same memory (ownership transfers to `v8::BackingStore`).

Note that passing a single backing store back and forth across the JS/C++ boundary multiple
times creates nested layers of `v8::BackingStore` and `kj::Array<kj::byte>` all pointing at
the same underlying allocation:

```
  std::shared_ptr<v8::BackingStore>
                |
        kj::Array<kj::byte>
                |
  std::shared_ptr<v8::BackingStore>
                |
        kj::Array<kj::byte>
                |
               ...
```

**`jsg::BufferSource`** — A more nuanced mapping that wraps the v8 handle of a given
`TypedArray` or `ArrayBuffer` and remembers its type. When passed back out to JavaScript, it
maps to exactly the same kind of object that was passed in (e.g. `Uint16Array` → `Uint16Array`).
The same `std::shared_ptr<v8::BackingStore>` is maintained. Also supports detaching the
backing store, which is important for ownership transfer. See the
[BackingStore & BufferSource](#21-backingstore-and-buffersource) advanced section for the
full API.

### Functions (`jsg::Function<Ret(Args...)>`)

`jsg::Function<Ret(Args...)>` provides a wrapper around JavaScript functions, making it easy
to call them from C++, store references to them, and wrap C++ lambdas as JavaScript functions.

**Taking a JS function and invoking it from C++:**

```cpp
void doSomething(jsg::Lock& js, jsg::Function<void(int)> callback) {
  callback(js, 1);
}
```

```js
doSomething((arg) => {
  console.log(arg);
}); // prints "1"
```

**Wrapping a C++ lambda as a JS function:**

```cpp
jsg::Function<void(int)> getFunction() {
  return jsg::Function<void(int)>([](jsg::Lock& js, int val) {
    KJ_DBG(val);
  });
}
```

```js
const func = getFunction();
func(1); // prints 1
```

### Promises (`jsg::Promise<T>`)

`jsg::Promise<T>` wraps a JavaScript promise with a syntax that makes it more natural and
ergonomic to consume within C++.

#### Creating Promises

```cpp
// Already-resolved promise
jsg::Promise<int> resolved = js.resolvedPromise(42);
jsg::Promise<void> resolvedVoid = js.resolvedPromise();

// Rejected promise
jsg::Promise<int> rejected = js.rejectedPromise<int>(js.error("Something went wrong"));

// Promise with resolver for later fulfillment
auto [promise, resolver] = js.newPromiseAndResolver<int>();
// Later...
resolver.resolve(js, 42);
// Or reject:
resolver.reject(js, js.error("Failed"));
```

#### Chaining with `.then()` and `.catch_()`

```cpp
jsg::Promise<int> promise = // ...

// Chain with both success and error handlers
promise.then(js, [](jsg::Lock& js, int value) {
  return value * 2;  // Returns jsg::Promise<int>
}, [](jsg::Lock& js, jsg::Value exception) {
  return 0;  // Must return the same type as the success handler
});

// Chain with only a success handler (errors propagate)
promise.then(js, [](jsg::Lock& js, int value) {
  return kj::str("Value: ", value);  // Returns jsg::Promise<kj::String>
});

// Chain with only an error handler
promise.catch_(js, [](jsg::Lock& js, jsg::Value exception) {
  return 0;
});
```

**Important:** Both handlers passed to `.then()` must return exactly the same type.

#### `Promise<void>`

```cpp
jsg::Promise<void> promise = // ...

promise.then(js, [](jsg::Lock& js) {
  // No value parameter for Promise<void>
});
```

#### The Resolver

`jsg::Promise<T>::Resolver` allows you to fulfill or reject a promise from elsewhere:

```cpp
class MyAsyncOperation {
  jsg::Promise<kj::String>::Resolver resolver;

public:
  jsg::Promise<kj::String> start(jsg::Lock& js) {
    auto [promise, r] = js.newPromiseAndResolver<kj::String>();
    resolver = kj::mv(r);
    return kj::mv(promise);
  }

  void complete(jsg::Lock& js, kj::String result) {
    resolver.resolve(js, kj::mv(result));
  }

  void fail(jsg::Lock& js, kj::Exception error) {
    resolver.reject(js, kj::mv(error));
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(resolver);  // Important for GC!
  }
};
```

#### `.whenResolved()` and `.consumeHandle()`

Unlike `.then()`, `whenResolved()` doesn't consume the promise and can be called multiple
times to create branches:

```cpp
jsg::Promise<void> done = promise.whenResolved(js);
```

To pass a promise to V8 APIs or return it directly:

```cpp
v8::Local<v8::Promise> handle = promise.consumeHandle(js);
```

After calling `consumeHandle()`, the `jsg::Promise` is consumed and cannot be used again.

#### Returning Promises from Methods

```cpp
class MyApi: public jsg::Object {
public:
  jsg::Promise<kj::String> fetchData(jsg::Lock& js) {
    auto [promise, resolver] = js.newPromiseAndResolver<kj::String>();
    // ... start async operation ...
    return kj::mv(promise);
  }

  JSG_RESOURCE_TYPE(MyApi) {
    JSG_METHOD(fetchData);
  }
};
```

### Symbols with `jsg::Name`

`jsg::Name` wraps values that can be used as property names in JavaScript (strings and
symbols). It is used when an API needs to accept either and treat them the same.

### Reference Types (`jsg::V8Ref<T>`, `jsg::Value`, `jsg::Ref<T>`)

#### `jsg::V8Ref<T>` (and `jsg::Value`, and `jsg::HashableV8Ref<T>`)

`jsg::V8Ref<T>` holds a persistent reference to a JavaScript type. The `T` must be one of
the `v8` types (e.g. `v8::Object`, `v8::Function`, etc). A `jsg::V8Ref<T>` maintains a
_strong_ reference, keeping the value from being garbage collected.

The `jsg::Value` type is a typedef alias for `jsg::V8Ref<v8::Value>`.

`jsg::V8Ref<T>` are reference counted. Multiple instances can share references to the same
underlying JavaScript value. When the last instance is destroyed, the underlying value is
freed to be garbage collected.

```cpp
jsg::V8Ref<v8::Boolean> boolRef1 = js.v8Ref(v8::True(js.v8Isolate));
jsg::V8Ref<v8::Boolean> boolRef2 = boolRef1.addRef(js);

KJ_DBG(boolRef1 == boolRef2);  // prints "true"

// Getting v8::Local<T> requires a v8::HandleScope on the stack:
v8::Local<v8::Boolean> boolLocal = js.withinHandleScope([&] {
  return boolRef1.getHandle(js);
});
```

`jsg::HashableV8Ref<T>` is a subclass that also implements `hashCode()` for use as a key
in a `kj::HashTable`.

#### `jsg::Ref<T>`

`jsg::Ref<T>` holds a persistent reference to a JSG Resource Type. It holds a _strong_
reference, keeping the resource from being garbage collected. It is reference counted.

Importantly, a resource type is a C++ object that _may_ have a corresponding JavaScript
"wrapper" object. This wrapper is created lazily when the `jsg::Ref<T>` instance is first
passed back out to JavaScript.

```cpp
jsg::Ref<Foo> foo = js.alloc<Foo>();
jsg::Ref<Foo> foo2 = foo.addRef();

js.withinHandleScope([&] {
  KJ_IF_SOME(handle, foo.tryGetHandle(js.v8Isolate)) {
    // handle is the Foo instance's JavaScript wrapper.
  }
});
```

### Memoized and Identified Types

#### `jsg::MemoizedIdentity<T>`

Typically, whenever a non-Resource Type is passed out to JavaScript, a _new_ JavaScript value
is created each time. `jsg::MemoizedIdentity<T>` preserves the JavaScript value so identity
is maintained across multiple passes:

```cpp
jsg::MemoizedIdentity<Foo> echoFoo(jsg::MemoizedIdentity<Foo> foo) {
  return kj::mv(foo);
}
```

```js
const foo = { value: 1 };
const foo2 = echoFoo(foo);
foo === foo2; // true — same object
```

#### `jsg::Identified<T>`

`jsg::Identified<T>` captures the identity of the JavaScript object that was passed, useful
for recognizing when the application passes the same value later:

```cpp
void doSomething(jsg::Identified<Foo> obj) {
  auto identity = obj.identity; // HashableV8Ref<v8::Object>
  Foo foo = obj.unwrapped;
}
```

---

## 4. Structured Types (`JSG_STRUCT`)

A JSG "struct" is a JavaScript object that can be mapped to a C++ struct:

```cpp
struct Foo {
  kj::String abc;
  jsg::Optional<bool> xyz;
  jsg::Value val;

  JSG_STRUCT(abc, xyz, val);

  int onlyInternal = 1;  // Not included in the JS mapping
};
```

Only properties listed in the `JSG_STRUCT` macro are mapped. When passing from JavaScript
into C++, additional properties on the object are ignored.

If the struct has a `validate()` method, it is called when the struct is unwrapped from v8:

```cpp
struct ValidatingFoo {
  kj::String abc;

  void validate(jsg::Lock& lock) {
    JSG_REQUIRE(abc.size() != 0, TypeError, "Field 'abc' had no length in 'ValidatingFoo'.");
  }

  JSG_STRUCT(abc);
};
```

To be used, JSG structs must be declared as part of the type system via
`JSG_DECLARE_ISOLATE_TYPE`.

---

## 5. Resource Types

A Resource Type is a C++ type mapped to a JavaScript object that is _not_ a plain object. It
is a JavaScript object backed by a corresponding C++ object.

Three key components:

- The underlying C++ type
- The JavaScript wrapper object
- The `v8::FunctionTemplate`/`v8::ObjectTemplate` that define the connection

### `jsg::Object`

All Resource Types must inherit from `jsg::Object`:

```cpp
class Foo: public jsg::Object {
public:
  Foo(int value): value(value) {}
  JSG_RESOURCE_TYPE(Foo) {}
private:
  int value;
};
```

### Constructors

For a Resource Type to be constructible from JavaScript, it must have a static `constructor()`
method returning `jsg::Ref<T>`:

```cpp
static jsg::Ref<Foo> constructor(jsg::Lock& js, int value) {
  return js.alloc<Foo>(value);
}
```

If this method is not provided, attempts to create new instances using `new ...` will fail
with an error.

### `JSG_RESOURCE_TYPE(T)`

All Resource Types must contain the `JSG_RESOURCE_TYPE(T)` macro. Within the block are
additional `JSG*` macros defining properties, methods, constants, etc. See the README for the
complete [JSG macro catalog](../src/workerd/jsg/README.md#jsg-macro-catalog).

### Methods

#### `JSG_METHOD(name)` and `JSG_METHOD_NAMED(name, method)`

Declares a method callable from JavaScript on instances. Methods are exposed on the prototype:

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();
  void bar();
  void delete_();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(bar);
    JSG_METHOD_NAMED(delete, delete_);  // JS name differs from C++ name
  }
};
```

```js
const foo = new Foo();
foo.bar();
foo.delete();

// Because it's on the prototype, subclasses can override:
class MyFoo extends Foo {
  delete() {
    super.delete();
  }
}
```

Arguments and return values are automatically marshalled using the JSG type mapping rules.

#### `JSG_STATIC_METHOD(name)` and `JSG_STATIC_METHOD_NAMED(name, method)`

Declares a method callable on the class itself:

```cpp
static void bar();
JSG_RESOURCE_TYPE(Foo) {
  JSG_STATIC_METHOD(bar);
}
```

```js
Foo.bar();
```

### Properties

Properties on JavaScript objects come in several varieties. See the README's
[property type decision matrix](../src/workerd/jsg/README.md#property-type-decision-matrix)
for guidance on which to use.

#### Prototype Properties (`JSG_READONLY_PROTOTYPE_PROPERTY`, `JSG_PROTOTYPE_PROPERTY`)

Defined on the prototype, shared by all instances, overridable by subclasses:

```cpp
kj::String getAbc();
int getXyz();
void setXyz(int value);

JSG_RESOURCE_TYPE(Foo) {
  JSG_READONLY_PROTOTYPE_PROPERTY(abc, getAbc);
  JSG_PROTOTYPE_PROPERTY(xyz, getXyz, setXyz);
}
```

```js
const foo = new Foo();
foo.abc; // read-only
foo.xyz = 456; // read-write
```

#### Instance Properties (`JSG_READONLY_INSTANCE_PROPERTY`, `JSG_INSTANCE_PROPERTY`)

Defined as own properties on each instance, _not_ overridable by subclasses:

```cpp
JSG_RESOURCE_TYPE(Foo) {
  JSG_READONLY_INSTANCE_PROPERTY(abc, getAbc);
  JSG_INSTANCE_PROPERTY(xyz, getXyz, setXyz);
}
```

While these _appear_ the same in simple cases, a subclass's `get abc()` override on the
prototype will not be called because the instance property shadows it.

**Note:** Prefer `JSG_PROTOTYPE_PROPERTY` over `JSG_INSTANCE_PROPERTY` unless you have a
specific reason — instance properties break GC optimization.

#### Lazy Instance Properties (`JSG_LAZY_READONLY_INSTANCE_PROPERTY`, `JSG_LAZY_INSTANCE_PROPERTY`)

Evaluated once and cached. Useful when a default value should be easily overridable by users
(typically used for introducing new global properties without breaking existing code):

```cpp
JSG_RESOURCE_TYPE(Foo) {
  JSG_LAZY_READONLY_INSTANCE_PROPERTY(abc, getAbc);
  JSG_LAZY_INSTANCE_PROPERTY(xyz, getXyz);
}
```

```js
foo.abc; // 'hello'
foo.abc = 1; // ignored (readonly)
foo.xyz; // 123
foo.xyz = 'hello'; // value type not enforced
foo.xyz; // 'hello'
```

### Static Constants

```cpp
static const int ABC = 123;
JSG_RESOURCE_TYPE(Foo) {
  JSG_STATIC_CONSTANT(ABC);
}
```

```js
Foo.ABC; // 123
```

### Iterable and Async Iterable Objects

Implemented using `JSG_ITERABLE`/`JSG_ASYNC_ITERABLE` macros with `JSG_ITERATOR`/`JSG_ASYNC_ITERATOR`:

```cpp
class Foo: public jsg::Object {
private:
  using EntryIteratorType = kj::Array<kj::String>;
  struct IteratorState final {
    void visitForGc(jsg::GcVisitor& visitor) { /* ... */ }
  };

public:
  static jsg::Ref<Foo> constructor();
  JSG_ITERATOR(Iterator, entries, EntryIteratorType, IteratorState, iteratorNext);

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(entries);
    JSG_ITERABLE(entries);
  }
};
```

```js
for (const entry of foo) {
  /* ... */
}
```

### Inheritance (`JSG_INHERIT`)

```cpp
class Foo: public Bar {
public:
  JSG_RESOURCE_TYPE(Foo) {
    JSG_INHERIT(Bar);
  }
};
```

```js
foo instanceof Bar; // true
```

### Nested Types (`JSG_NESTED_TYPE`, `JSG_NESTED_TYPE_NAMED`)

Exposes the constructor of a nested Resource Type:

```cpp
JSG_RESOURCE_TYPE(Foo) {
  JSG_NESTED_TYPE(Bar);
  JSG_NESTED_TYPE_NAMED(OtherBar, Baz);
}
```

```js
const bar = new Foo.Bar();
const baz = new Foo.Baz();
```

### Callable Objects (`JSG_CALLABLE`)

Makes a Resource Type callable as a function:

```cpp
class MyAssert: public jsg::Object {
public:
  void ok(boolean condition) {
    JSG_REQUIRE(condition, Error, "Condition failed!");
  }
  JSG_RESOURCE_TYPE(MyAssert) {
    JSG_CALLABLE(ok);
    JSG_METHOD(ok);
  }
};
```

```js
assert.ok(true); // method call
assert(true); // function call (same as ok)
```

### Compatibility Flags

`JSG_RESOURCE_TYPE` accepts an optional `CompatibilityFlags::Reader` parameter to
conditionally expose APIs:

```cpp
JSG_RESOURCE_TYPE(MyApi, workerd::CompatibilityFlags::Reader flags) {
  JSG_METHOD(oldMethod);  // Always exposed
  if (flags.getMyNewFeature()) {
    JSG_METHOD(newMethod);  // Only with flag enabled
  }
}
```

The compatibility flags are defined in `src/workerd/io/compatibility-date.capnp`.

---

## 6. Type System Declaration

For JSG's type system to function, you must declare all types via `JSG_DECLARE_ISOLATE_TYPE`.
Every resource type and `JSG_STRUCT` type must be listed:

```cpp
JSG_DECLARE_ISOLATE_TYPE(MyIsolate, api::Foo, api::Bar)
```

In workerd, you'll see a pattern using `EW_*_ISOLATE_TYPES` macros:

```cpp
JSG_DECLARE_ISOLATE_TYPE(JsgServeIsolate,
  EW_GLOBAL_SCOPE_ISOLATE_TYPES,
  EW_ACTOR_ISOLATE_TYPES,
  EW_ACTOR_STATE_ISOLATE_TYPES,
  EW_ANALYTICS_ENGINE_ISOLATE_TYPES,
  ...
```

Each `EW_*_ISOLATE_TYPES` macro is defined in its respective header file as a shortcut.
`JSG_DECLARE_ISOLATE_TYPE` should only be defined once in your application.

---

## 7. Memory Management

### Garbage Collection and GC Visitation

V8's garbage collector is mark-and-sweep: it marks all reachable objects from the root set,
then frees unmarked objects. When using `jsg::V8Ref<T>` or `jsg::Ref<T>` to hold references,
you must mark reachable objects so the GC knows how to handle them.

A Resource Type containing GC-visitable fields should implement `visitForGc()`:

```cpp
class Foo: public jsg::Object {
public:
  jsg::Ref<Bar> getBar();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(bar, getBar);
  }

private:
  jsg::Ref<Bar> bar;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(bar);
  }
};
```

JSG automatically detects the `visitForGc()` implementation. If your Resource Type owns no
ref types, implementing it is not necessary. As a best practice, implement it on all types
that contain refs, regardless of whether reference cycles are a concern.

### Ownership Rules

For `jsg::Ref<T>`, garbage collection only applies to the JavaScript wrapper. From C++'s
perspective, `jsg::Ref<T>` is a strong reference to a refcounted object. Unreachable cycles
of `jsg::Ref<T>` will have their JavaScript objects collected, but the C++ objects will not
be freed until the cycle is broken.

**Critical:** Think about ownership and only hold `jsg::Ref<T>` from owner to owned objects.
"Backwards" references (owned→owner) should be regular C++ references or pointers. If the
reference might be nulled out, use `kj::Maybe<T&>`.

See the README for the complete [GC-visitable types table](../src/workerd/jsg/README.md#gc-visitable-types).

---

## 8. JsValue Types

The `JsValue` family provides a modern abstraction layer over V8's `v8::Local<T>` handles.
See the README for the complete [JsValue type mapping table](../src/workerd/jsg/README.md#jsvalue-type-mapping).

### Key Characteristics

1. **Stack-only allocation** — enforced in debug builds. Lightweight wrappers around V8 handles.
2. **Implicit conversion to V8 types** — all `JsValue` types convert to their `v8::Local<T>`.
3. **Implicit conversion to `JsValue`** — all specific types upcast to `JsValue`.
4. **Use `JsRef<T>` for persistence** — to store a value beyond the current scope.

### Creating Values with `jsg::Lock`

```cpp
void createValues(jsg::Lock& js) {
  // Primitives
  JsValue undef = js.undefined();
  JsValue null = js.null();
  JsBoolean b = js.boolean(true);

  // Numbers
  JsNumber n = js.num(3.14);
  JsInt32 i32 = js.num(42);
  JsBigInt big = js.bigInt(INT64_MAX);

  // Strings
  JsString s = js.str("hello");
  JsString interned = js.strIntern("propertyName");

  // Objects and collections
  JsObject obj = js.obj();
  JsObject noProto = js.objNoProto();
  JsMap map = js.map();
  JsArray arr = js.arr(js.num(1), js.num(2), js.num(3));
  JsSet set = js.set(js.str("a"), js.str("b"));

  // Symbols, Dates, Errors
  JsSymbol sym = js.symbol("mySymbol");
  JsDate date = js.date(1234567890.0);
  JsValue err = js.error("Something went wrong");
  JsValue typeErr = js.typeError("Expected a string");

  // Global object
  JsObject global = js.global();
}
```

### Type Checking and Casting

```cpp
void checkTypes(jsg::Lock& js, JsValue value) {
  if (value.isString()) { /* ... */ }
  if (value.isNullOrUndefined()) { /* ... */ }

  KJ_IF_SOME(str, value.tryCast<JsString>()) {
    kj::String cppStr = str.toString(js);
  }
}
```

### Working with Objects

```cpp
void objectOps(jsg::Lock& js) {
  JsObject obj = js.obj();

  obj.set(js, "name", js.str("Alice"));
  obj.setReadOnly(js, "version", js.str("1.0"));
  JsValue name = obj.get(js, "name");

  if (obj.has(js, "name", JsObject::HasOption::OWN)) { /* own property */ }
  obj.delete_(js, "name");

  JsArray names = obj.getPropertyNames(js,
      KeyCollectionFilter::OWN_ONLY,
      PropertyFilter::ONLY_ENUMERABLE,
      IndexFilter::INCLUDE_INDICES);

  if (obj.isInstanceOf<MyResourceType>(js)) {
    auto ref = KJ_ASSERT_NONNULL(obj.tryUnwrapAs<MyResourceType>(js));
  }

  obj.seal(js);
  obj.recursivelyFreeze(js);
  JsObject clone = obj.jsonClone(js);
}
```

### Working with Strings

```cpp
void stringOps(jsg::Lock& js) {
  JsString str = js.str("Hello, World!");
  int len = str.length(js);           // UTF-16 code units
  size_t utf8Len = str.utf8Length(js); // UTF-8 bytes
  kj::String cppStr = str.toString(js);

  // Check string properties
  bool flat = str.isFlat();               // True if contiguous in memory
  bool oneByte = str.containsOnlyOneByte(); // True if all chars fit in one byte

  JsString combined = JsString::concat(js, js.str("Hello, "), js.str("World!"));
  JsString interned = str.internalize(js);

  // Write to buffer
  kj::Array<char> buf = kj::heapArray<char>(100);
  auto status = str.writeInto(js, buf, JsString::WriteFlags::NULL_TERMINATION);
  // status.read = characters read from string
  // status.written = bytes written to buffer
}
```

### Working with Arrays and Functions

```cpp
void arrayOps(jsg::Lock& js) {
  JsArray arr = js.arr(js.num(1), js.num(2), js.num(3));
  uint32_t len = arr.size();
  JsValue elem = arr.get(js, 0);
  arr.add(js, js.str("new item"));
}

void functionOps(jsg::Lock& js, JsFunction func) {
  JsValue result = func.call(js, js.global(), js.num(1), js.str("arg"));
  JsValue result2 = func.callNoReceiver(js, js.num(42));
  size_t length = func.length(js);
  JsString name = func.name(js);
}
```

### JSON and Structured Clone

```cpp
void jsonOps(jsg::Lock& js, JsValue value) {
  kj::String json = value.toJson(js);
  JsValue parsed = JsValue::fromJson(js, R"({"key": "value"})");
}

void cloneOps(jsg::Lock& js, JsValue value) {
  JsValue cloned = value.structuredClone(js);
  // Clone with transferables:
  kj::Array<JsValue> transfers = kj::heapArray<JsValue>({someArrayBuffer});
  JsValue cloned2 = value.structuredClone(js, kj::mv(transfers));
}
```

### Persisting Values with `JsRef<T>`

`JsValue` types are only valid within the current scope. To store a value persistently:

```cpp
class MyClass: public jsg::Object {
public:
  void storeValue(jsg::Lock& js, JsValue value) {
    stored = value.addRef(js);
  }

  JsValue getStored(jsg::Lock& js) {
    return stored.getHandle(js);
  }

private:
  JsRef<JsValue> stored;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(stored);  // Important for garbage collection!
  }
};
```

### Comparison and Equality

```cpp
bool equal = (a == b);              // Abstract equality (==)
bool strictEqual = a.strictEquals(b); // Strict equality (===)
bool truthy = a.isTruthy(js);
kj::String type = a.typeOf(js);     // "string", "number", etc.
```

### `JsPromise` vs `jsg::Promise<T>`

- **`JsPromise`**: Thin wrapper around `v8::Promise` for inspecting state. Cannot be awaited
  in C++. Use when you need to check pending/fulfilled/rejected or access the result directly.
- **`jsg::Promise<T>`**: Higher-level abstraction with `.then()`, `.catch_()` and JSG type
  system integration. Use this for most promise handling.

---

## 9. Serialization

JSG provides serialization built on V8's `ValueSerializer` supporting the
[structured clone algorithm](https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm).

### Basic Usage

```cpp
JsValue cloned = jsg::structuredClone(js, value);

// With transferables:
kj::Array<JsValue> transfers = kj::heapArray<JsValue>({someArrayBuffer});
JsValue cloned2 = jsg::structuredClone(js, value, kj::mv(transfers));
```

### Serializer and Deserializer

```cpp
// Serialize
Serializer::Released serialized = ({
  Serializer ser(js);
  ser.write(js, value);
  ser.release();
});
// serialized.data, .sharedArrayBuffers, .transferredArrayBuffers

// Deserialize
JsValue result = ({
  Deserializer deser(js, serialized);
  deser.readValue(js);
});
```

Multiple values can be serialized/deserialized in sequence.

#### Serializer Options

```cpp
Serializer::Options options {
  .version = kj::none,
  .omitHeader = false,
  .treatClassInstancesAsPlainObjects = true,
  .externalHandler = kj::none,
};
Serializer ser(js, options);
```

#### Transferring ArrayBuffers

```cpp
Serializer ser(js);
ser.transfer(js, buffer);  // Mark for transfer before writing
ser.write(js, buffer);
auto released = ser.release();
// buffer is now detached (neutered)
```

### Making Resource Types Serializable (`JSG_SERIALIZABLE`)

```cpp
enum class SerializationTag {
  MY_TYPE_V1 = 1,
  MY_TYPE_V2 = 2,
};

class MyType: public jsg::Object {
public:
  MyType(uint32_t id, kj::String name): id(id), name(kj::mv(name)) {}

  JSG_RESOURCE_TYPE(MyType) {
    JSG_READONLY_PROTOTYPE_PROPERTY(id, getId);
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    serializer.writeRawUint32(id);
    serializer.writeLengthDelimited(name);
  }

  static jsg::Ref<MyType> deserialize(
      jsg::Lock& js, SerializationTag tag, jsg::Deserializer& deserializer) {
    uint32_t id = deserializer.readRawUint32();
    kj::String name = deserializer.readLengthDelimitedString();
    return js.alloc<MyType>(id, kj::mv(name));
  }

  // MUST appear AFTER the JSG_RESOURCE_TYPE block, not inside it
  JSG_SERIALIZABLE(SerializationTag::MY_TYPE_V1);

private:
  uint32_t id;
  kj::String name;
};
```

**Important notes:**

- `JSG_SERIALIZABLE` must appear **after** the `JSG_RESOURCE_TYPE` block
- Tag enum values must never change once data has been serialized
- `deserialize()` receives the tag so it can handle multiple versions

### Versioning Serializable Types

List the current tag first, then old tags:

```cpp
// V2 adds optional description
void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  serializer.writeRawUint32(id);
  serializer.writeLengthDelimited(name);
  KJ_IF_SOME(desc, description) {
    serializer.writeRawUint32(1);
    serializer.writeLengthDelimited(desc);
  } else {
    serializer.writeRawUint32(0);
  }
}

static jsg::Ref<MyType> deserialize(
    jsg::Lock& js, SerializationTag tag, jsg::Deserializer& deserializer) {
  uint32_t id = deserializer.readRawUint32();
  kj::String name = deserializer.readLengthDelimitedString();
  kj::Maybe<kj::String> description;
  if (tag == SerializationTag::MY_TYPE_V2) {
    if (deserializer.readRawUint32() == 1) {
      description = deserializer.readLengthDelimitedString();
    }
  }
  return js.alloc<MyType>(id, kj::mv(name), kj::mv(description));
}

JSG_SERIALIZABLE(SerializationTag::MY_TYPE_V2, SerializationTag::MY_TYPE_V1);
```

### TypeHandlers in Serialization

Both `serialize()` and `deserialize()` can request `TypeHandler` arguments:

```cpp
void serialize(jsg::Lock& js, jsg::Serializer& serializer,
               const jsg::TypeHandler<kj::String>& stringHandler) {
  serializer.write(js, JsValue(stringHandler.wrap(js, kj::str(text))));
}
```

### Raw Read/Write Methods

See the README's [serialization pattern](../src/workerd/jsg/README.md#serialization-pattern)
for the complete list of raw serializer/deserializer methods.

### One-Way Serialization (`JSG_SERIALIZABLE_ONEWAY`)

For types that serialize to a different type (e.g., a legacy type deserializing as a newer
type):

```cpp
class LegacyType: public jsg::Object {
  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    serializer.writeRawUint32(value);
  }
  // No deserialize() — uses NewType's deserializer
  JSG_SERIALIZABLE_ONEWAY(SerializationTag::NEW_TYPE);
};
```

---

## 10. Error Handling

### Error Macros

```cpp
JSG_REQUIRE(condition, TypeError, "Expected a valid value, got ", value);
auto& val = JSG_REQUIRE_NONNULL(maybeValue, TypeError, "Value must not be null");
JSG_FAIL_REQUIRE(RangeError, "Index ", index, " is out of bounds");
JSG_ASSERT(condition, Error, "Internal assertion failed");
```

The error type can be: `TypeError`, `Error`, `RangeError`, or DOMException types like
`DOMOperationError`, `DOMDataError`, `DOMInvalidStateError`, etc. See the README for the
full [error type catalog](../src/workerd/jsg/README.md#error-type-catalog).

Unlike `KJ_REQUIRE`, `JSG_REQUIRE` passes all message arguments through `kj::str()`, so you
are responsible for formatting the entire message string.

### `js.error()`, `js.throwException()`, and `JsExceptionThrown`

```cpp
void someMethod(jsg::Lock& js) {
  if (somethingWrong) {
    js.throwException(js.error("Something went wrong"));
  }
}
```

Under the hood, `js.throwException()` uses `isolate->ThrowException()` then throws
`JsExceptionThrown` to unwind the C++ stack back to the JS/C++ boundary.

### `JSG_TRY` and `JSG_CATCH`

Replace normal `try`/`catch` when you need to catch exceptions as JavaScript exceptions:

```cpp
JSG_TRY(js) {
  someThrowyCode();
}
JSG_CATCH(e) {
  // 'e' is a JsValue wrapping the JavaScript error
  // Useful for coercing any C++ exception into a JavaScript Error:
  js.throwException(kj::mv(e));
}
```

**Important:** `JSG_CATCH` is NOT a true catch — you cannot rethrow with `throw`.

### `makeInternalError()` and `throwInternalError()`

Create JavaScript errors from internal C++ exceptions while obfuscating sensitive
implementation details. If the exception was created using `throwTunneledException()`, the
original JavaScript exception is reconstructed instead.

### `throwTypeError()`

Throws a JavaScript `TypeError` with contextual information:

```cpp
throwTypeError(isolate, TypeErrorContext::methodArgument(typeid(MyClass), "doThing", 0),
               "string");
throwTypeError(isolate, "Expected a string but got a number"_kj);
```

### `check()`

Unwraps V8's `MaybeLocal` and `Maybe` types, throwing `JsExceptionThrown` if empty:

```cpp
v8::Local<v8::String> str = check(maybeStr);
```

### `jsg::DOMException`

Implements the standard Web IDL [DOMException][] interface. To throw from C++:

```cpp
JSG_REQUIRE(isValid, DOMInvalidStateError, "The object is in an invalid state");
JSG_REQUIRE(hasAccess, DOMNotSupportedError, "This operation is not supported");
```

### Tunneled Exceptions

JavaScript exceptions can be "tunneled" through KJ's exception system — thrown, caught as
`kj::Exception`, passed across boundaries (like RPC), and reconstructed back:

```cpp
kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);
[[noreturn]] void throwTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);

// Check if a kj::Exception contains a tunneled JS exception:
if (jsg::isTunneledException(exception.getDescription())) { /* ... */ }
```

The tunneling mechanism encodes the exception type and message in a special format using
prefixes like `jsg.TypeError:` or `jsg.DOMException(NotFoundError):`.

[DOMException]: https://webidl.spec.whatwg.org/#idl-DOMException

---

## 11. Utility Functions

### `jsg::v8Str(...)` and `jsg::v8StrIntern(...)`

Create V8 string values from C++ strings:

```cpp
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr str);
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::ArrayPtr<const char16_t> ptr);
v8::Local<v8::String> v8StrIntern(v8::Isolate* isolate, kj::StringPtr str);
```

**Note:** New code should prefer `js.str()` and `js.strIntern()` instead.

### External Strings

For static constant strings that will never be deallocated:

```cpp
v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf);
v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf);
```

**Important:** The `OneByteString` variant interprets the buffer as Latin-1, not UTF-8.

---

## 12. V8 Fast API

V8 Fast API allows V8 to compile JavaScript code that calls native functions by generating
specialized machine code that directly calls the C++ implementation, skipping the usual
binding layers.

### Requirements

1. **Return type**: `void`, `bool`, `int32_t`, `uint32_t`, `float`, or `double`
2. **Parameter types**: The primitives above, V8 handle types, or TypeWrapper-unwrappable types
3. **Method structure**: Regular/const instance method, optionally with `jsg::Lock&` first param

### Usage

By default, any `JSG_METHOD(name)` executes the fast path if the method signature is
compatible. To explicitly assert compatibility:

```cpp
JSG_ASSERT_FASTAPI(MyClass::myMethod);
```

V8 determines at runtime whether to use the fast or slow path based on whether the call
originates from optimized code.

---

## 13. TypeScript Generation

TypeScript definitions are automatically generated from JSG RTTI using scripts in `/types`.
Three macros control auto-generation inside `JSG_RESOURCE_TYPE` blocks, plus struct variants.

### `JSG_TS_ROOT` / `JSG_STRUCT_TS_ROOT`

Declares a type as a "root" for TypeScript generation. All roots and their recursively
referenced types are included. Roots exist because some types should only be included when
certain compatibility flags are enabled.

Note roots are visited before overrides, so if an override references a new type that wasn't
already referenced, it needs to be declared a root itself.

### `JSG_TS_OVERRIDE` / `JSG_STRUCT_TS_OVERRIDE`

Customises the generated TypeScript definition. Accepts a partial TypeScript statement.
See the README's [JSG_TS_OVERRIDE rules](../src/workerd/jsg/README.md#jsg_ts_override-rules)
for the complete rule set.

Examples:

```ts
// Rename type
KVNamespaceListOptions

// Replace method, keep others
{ json<T>(): Promise<T> }

// Add type parameter and replace methods
<R = any> {
  read(): Promise<ReadableStreamReadResult<R>>;
  tee(): [ReadableStream<R>, ReadableStream<R>];
}

// Remove a member
{ actorState: never }

// Replace heritage
extends EventTarget<WorkerGlobalScopeEventMap>

// Full replacement
class Body { json<T>(): Promise<T> }

// Delete definition
type TransactionOptions = never
```

These macros can be called conditionally based on compatibility flags. For compatibility-flag
dependent `JSG_STRUCT` overrides, delete the original with a `never` type alias, then use
`JSG_TS_DEFINE` in a nearby `JSG_RESOURCE_TYPE` to define an interface conditionally.

### `JSG_TS_DEFINE` / `JSG_STRUCT_TS_DEFINE`

Inserts additional TypeScript definitions next to the generated definition. Can only be used
once per block. The `declare` modifier is automatically added to `class`, `enum`, `const`,
`var`, and `function` definitions.

---

## 14. Async Context Tracking

JSG provides rudimentary async context tracking via `AsyncContextFrame` in
`src/workerd/jsg/async-context.h`, supporting the implementation of async local storage.

`AsyncContextFrame`s form a logical stack. For every `v8::Isolate` there is always a root
frame. Each frame has a storage context (a map of storage cells keyed by opaque keys).
When a new frame is created, the current frame's storage context is propagated.

All JavaScript promises, timers, and microtasks propagate the async context.

```js
import { default as async_hooks } from 'node:async_hooks';
const { AsyncLocalStorage } = async_hooks;

const als = new AsyncLocalStorage();
als
  .run(123, () => scheduler.wait(10))
  .then(() => {
    console.log(als.getStore()); // 123
  });
console.log(als.getStore()); // undefined
```

### C++ Usage

```cpp
// Capture the current async context
auto maybeAsyncContext = jsg::AsyncContextFrame::current(js);
// or jsg::AsyncContextFrame::currentRef(js) for a jsg::Ref

// Enter the async resource scope
{
  jsg::AsyncContextFrame::Scope asyncScope(js, maybeAsyncContext);
  // run synchronous code that requires the async context
}
// Scope exits automatically
```

### StorageScope (independent of Node.js API)

```cpp
kj::Own<AsyncContextFrame::StorageKey> key =
    kj::refcounted<AsyncContextFrame::StorageKey>();
KJ_DEFER(key->reset());  // Clear when done

{
  jsg::AsyncContextFrame::StorageScope(js, *key, value);
  // code runs with this storage context
}
// Automatically reset to previous context
```

---

## 15. Memory Tracking (`jsg::MemoryTracker`)

Integrates with V8's `BuildEmbedderGraph` API to include C++ object data in heap snapshots.

A type must implement at least:

- `kj::StringPtr jsgGetMemoryName() const;` — name in graph (prefixed `"workerd / "`)
- `size_t jsgGetMemorySelfSize() const;` — shallow size (typically `sizeof(Type)`)
- `void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;` — collect details

The `JSG_MEMORY_INFO` macro provides shorthand:

```cpp
JSG_MEMORY_INFO(Foo) {
  tracker.trackField("bar", bar);
}
```

For `jsg::Object` subclasses, implement `visitForMemoryInfo()` instead:

```cpp
class Foo : public jsg::Object {
public:
  JSG_RESOURCE_TYPE(Foo) {}
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("bar", bar);
  }
};
```

Optional additional methods: `jsgGetMemoryInfoWrapperObject()`,
`jsgGetMemoryInfoDetachedState()`, `jsgGetMemoryInfoIsRootNode()`.

This code is only called during heap snapshot generation; it should have very little cost and
ideally should not allocate.

---

## 16. Script Utilities

### `jsg::NonModuleScript`

Wraps a `v8::UnboundScript` — compiled but not yet bound to a specific context:

```cpp
#include <workerd/jsg/script.h>

auto script = jsg::NonModuleScript::compile(js, "console.log('Hello!'); return 42;",
                                            "my-script.js");
script.run(js);
jsg::JsValue result = script.runAndReturn(js);
```

Scripts can be compiled once and run multiple times. Each run binds to the current context.

---

## 17. URL Utilities

JSG provides WHATWG-compliant URL parsing in `url.h`, powered by the ada-url library.

### `jsg::Url`

```cpp
KJ_IF_SOME(url, jsg::Url::tryParse("https://example.com:8080/path?query=1#hash")) {
  kj::ArrayPtr<const char> protocol = url.getProtocol();  // "https:"
  kj::ArrayPtr<const char> hostname = url.getHostname();  // "example.com"
  kj::ArrayPtr<const char> pathname = url.getPathname();  // "/path"

  url.setPathname("/new/path");
  kj::ArrayPtr<const char> href = url.getHref();
}

// Parse with a base URL
KJ_IF_SOME(resolved, jsg::Url::tryParse("../other", "https://example.com/path/")) {
  // resolved is "https://example.com/other"
}

// Check validity without creating a Url object
if (jsg::Url::canParse("https://example.com")) { /* valid */ }

// Literal operator
jsg::Url url = "https://example.com"_url;
```

#### URL Comparison

```cpp
using Option = jsg::Url::EquivalenceOption;
if (a.equal(b, Option::IGNORE_FRAGMENTS)) { /* same ignoring #hash */ }
if (a.equal(b, Option::IGNORE_FRAGMENTS | Option::IGNORE_SEARCH)) { /* ... */ }
```

### `jsg::UrlSearchParams`

```cpp
KJ_IF_SOME(params, jsg::UrlSearchParams::tryParse("foo=1&bar=2&foo=3")) {
  KJ_IF_SOME(value, params.get("foo")) { /* "1" (first occurrence) */ }
  auto allFoo = params.getAll("foo");  // ["1", "3"]

  // Check existence
  bool hasFoo = params.has("foo");
  bool hasFooWithValue = params.has("foo", "1"_kj);

  // Modify
  params.append("baz", "4");
  params.set("foo", "new");  // Replaces all "foo" entries
  params.delete_("bar");

  // Iterate
  auto keys = params.getKeys();
  while (keys.hasNext()) {
    KJ_IF_SOME(key, keys.next()) { /* ... */ }
  }

  params.sort();
  kj::Array<const char> str = params.toStr();  // "baz=4&foo=new"
}
```

### `jsg::UrlPattern`

```cpp
auto result = jsg::UrlPattern::tryCompile("/users/:id");
KJ_SWITCH_ONEOF(result) {
  KJ_CASE_ONEOF(pattern, jsg::UrlPattern) {
    auto& pathname = pattern.getPathname();
    kj::StringPtr regex = pathname.getRegex();
    auto names = pathname.getNames();  // ["id"]
  }
  KJ_CASE_ONEOF(error, kj::String) {
    KJ_LOG(ERROR, "Invalid pattern", error);
  }
}

// Compile with options
jsg::UrlPattern::CompileOptions options {
  .baseUrl = "https://example.com",
  .ignoreCase = true,
};
auto result2 = jsg::UrlPattern::tryCompile("/path", options);
```

---

## 18. RTTI (Runtime Type Information)

The RTTI system in `rtti.h` introspects JSG types at runtime and produces Cap'n Proto
descriptions (schema in `rtti.capnp`). Used for TypeScript generation, dynamic invocation,
fuzzing, and backward compatibility checks.

### Using the RTTI Builder

```cpp
#include <workerd/jsg/rtti.h>

jsg::rtti::Builder<Config> rtti(config);
auto intType = rtti.type<int>();
auto myClassInfo = rtti.structure<MyClass>();

for (auto member : myClassInfo.getMembers()) {
  KJ_LOG(INFO, "Member:", member.getName());
}

// Lookup structure by name (returns Maybe)
KJ_IF_SOME(structInfo, rtti.structure("MyClass"_kj)) {
  // Use structInfo...
}
```

### TypeScript Generation Process

1. Instantiate RTTI builder with appropriate configuration
2. Iterate through all registered types
3. Convert Cap'n Proto metadata to TypeScript syntax
4. Apply any `JSG_TS_OVERRIDE` customizations

See the README's [RTTI type mapping table](../src/workerd/jsg/README.md#rtti-type-mapping)
for the complete mapping.

---

## 19. Wrappable Internals

### The `Wrappable` Base Class

`Wrappable` is the base class for all C++ objects exposed to JavaScript. It manages the
connection between a C++ object and its JavaScript "wrapper" object.

#### Key Concepts

1. **Lazy Wrapper Creation** — Wrappers are created on-demand when a C++ object is first
   passed to JavaScript, not when constructed.

2. **Dual Reference Counting** — The `Wrappable` is ref-counted via `kj::Refcounted` (JS
   wrapper holds a reference). A second "strong ref" count tracks `jsg::Ref<T>` pointers not
   visible to GC tracing.

3. **Identity Preservation** — The same C++ object always returns the same JS wrapper,
   preserving object identity and monkey-patches.

#### Internal Fields

JavaScript wrapper objects have two internal fields:

```cpp
enum InternalFields : int {
  WRAPPABLE_TAG_FIELD_INDEX = 0,    // Contains WORKERD_WRAPPABLE_TAG
  WRAPPED_OBJECT_FIELD_INDEX = 1,   // Pointer back to the Wrappable
  INTERNAL_FIELD_COUNT = 2,
};
```

Check if an object is a workerd API object: `jsg::Wrappable::isWorkerdApiObject(object)`

#### Context Embedder Data Slots

See the README's [context embedder data slots table](../src/workerd/jsg/README.md#context-embedder-data-slots).

```cpp
jsg::setAlignedPointerInEmbedderData(context, ContextPointerSlot::MODULE_REGISTRY, registry);
KJ_IF_SOME(registry, jsg::getAlignedPointerFromEmbedderData<ModuleRegistry>(
    context, ContextPointerSlot::MODULE_REGISTRY)) {
  // Use registry...
}
```

### `HeapTracer`

Implements V8's `EmbedderRootsHandler` to integrate JSG's C++ object graph with V8's GC.
Tracks all `Wrappable` objects with JS wrappers, decides which can be collected, and manages
a freelist of reusable wrapper shim objects.

```cpp
if (jsg::HeapTracer::isInCppgcDestructor()) {
  // Be careful — we're being destroyed during GC
}
```

### Wrapper Lifecycle

```
1. C++ object created (no JS wrapper yet)
         |
2. Object passed to JavaScript
         |
3. attachWrapper() creates JS wrapper
         |
4. JS wrapper and C++ object linked
         |
5. GC may collect wrapper if:
   - No JS references exist
   - No strong Ref<T>s exist
   - Wrapper is "unmodified"
         |
6. If wrapper collected but C++ object still alive:
   - New wrapper created on next JS access
         |
7. When C++ object destroyed:
   - detachWrapper() called
   - JS wrapper becomes empty shell
```

### Async Destructor Safety

JSG enforces that JavaScript heap objects don't hold KJ I/O objects directly:

```cpp
DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;
// If you need to store I/O objects, use IoOwn<T>
```

---

## 20. Observers

JSG provides an observer system for monitoring runtime events.

### `IsolateObserver`

The main observer interface combining `CompilationObserver`, `ResolveObserver`, and
`InternalExceptionObserver`:

```cpp
class MyObserver: public jsg::IsolateObserver { /* ... */ };
auto observer = kj::heap<MyObserver>();
MyIsolate isolate(v8System, kj::mv(observer));
```

### Dynamic Code Generation Monitoring

```cpp
void onDynamicEval(v8::Local<v8::Context> context, v8::Local<v8::Value> source,
                   jsg::IsCodeLike isCodeLike) override {
  KJ_LOG(WARNING, "Dynamic code generation detected");
}
```

### `CompilationObserver`

`onXxxStart` methods return `kj::Own<void>` destroyed when compilation completes (RAII
timing):

```cpp
kj::Own<void> onEsmCompilationStart(v8::Isolate* isolate, kj::StringPtr name,
                                     Option option) const override {
  auto startTime = kj::systemCoarseMonotonicClock().now();
  return kj::defer([startTime, name = kj::str(name)]() {
    auto duration = kj::systemCoarseMonotonicClock().now() - startTime;
    KJ_LOG(INFO, "ESM compilation", name, duration / kj::MILLISECONDS, "ms");
  });
}
```

Compilation `Option`: `BUNDLE` (user code) or `BUILTIN` (runtime modules).

Other `CompilationObserver` hooks: `onScriptCompilationStart`, `onWasmCompilationStart`,
`onWasmCompilationFromCacheStart`, `onJsonCompilationStart`, `onCompileCacheFound`,
`onCompileCacheRejected`, `onCompileCacheGenerated`, `onCompileCacheGenerationFailed`.

### `ResolveObserver`

Monitors module resolution:

```cpp
kj::Own<ResolveStatus> onResolveModule(kj::StringPtr specifier, Context context,
                                        Source source) const override { /* ... */ }
```

- `Context`: `BUNDLE`, `BUILTIN`, `BUILTIN_ONLY`
- `Source`: `STATIC_IMPORT`, `DYNAMIC_IMPORT`, `REQUIRE`, `INTERNAL`

`ResolveStatus` callbacks: `found()`, `notFound()`, `exception()`.

### `InternalExceptionObserver`

```cpp
void reportInternalException(const kj::Exception& exception, Detail detail) override {
  // detail.isInternal, detail.isFromRemote, detail.isDurableObjectReset, detail.internalErrorId
}
```

---

## 21. BackingStore and BufferSource

### `jsg::BackingStore`

Wraps `v8::BackingStore` with type information. Once allocated, can be safely used outside
the isolate lock.

#### Creating

```cpp
// From kj::Array (takes ownership)
auto backing = jsg::BackingStore::from<v8::Uint8Array>(js, kj::mv(data));

// Allocate new zero-initialized buffer
auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, 1024);

// Wrap external data with custom disposer
auto backing = jsg::BackingStore::wrap<v8::Uint8Array>(
    externalData, 1024,
    [](void* data, size_t len, void* ctx) { free(data); },
    nullptr);
```

Template parameter specifies TypedArray type for JavaScript conversion.

#### Accessing Data

```cpp
kj::ArrayPtr<kj::byte> bytes = backing.asArrayPtr();
kj::ArrayPtr<uint32_t> u32s = backing.asArrayPtr<uint32_t>();
size_t size = backing.size();
size_t offset = backing.getOffset();
size_t elemSize = backing.getElementSize();
bool isInt = backing.isIntegerType();
```

#### Manipulating Views

```cpp
auto uint16View = backing.getTypedView<v8::Uint16Array>();
auto slice = backing.getTypedViewSlice<v8::Uint8Array>(10, 100);
backing.consume(10);  // Skip first 10 bytes
backing.trim(10);     // Remove last 10 bytes
backing.limit(100);   // Cap at 100 bytes
auto cloned = backing.clone();  // Shares buffer
auto copied = backing.copy<v8::Uint8Array>(js);  // New buffer
```

#### Converting Back to JavaScript

```cpp
v8::Local<v8::Value> handle = backing.createHandle(js);
```

### `jsg::BufferSource`

Wraps a JavaScript ArrayBuffer or ArrayBufferView, retaining the original reference and
supporting detachment.

```cpp
class MyApi: public jsg::Object {
public:
  jsg::BufferSource processData(jsg::Lock& js, jsg::BufferSource source) {
    jsg::BackingStore backing = source.detach(js);
    // Process backing data...
    return jsg::BufferSource(js, kj::mv(backing));
  }
};
```

#### Creating

```cpp
jsg::BufferSource source1(js, kj::mv(backing));     // From BackingStore
jsg::BufferSource source2(js, jsValue);              // From JS handle
KJ_IF_SOME(s, jsg::BufferSource::tryAlloc(js, 1024)) { /* ... */ }
jsg::BufferSource source5 = js.arrayBuffer(kj::mv(data));  // From kj::Array
```

#### Detaching

```cpp
if (!source.isDetached() && source.canDetach(js)) {
  jsg::BackingStore backing = source.detach(js);
  // Original JS ArrayBuffer is now neutered (zero-length)
}
```

Detach keys for security:

```cpp
v8::Local<v8::Value> key = js.str("secret-key");
source.setDetachKey(js, key);
jsg::BackingStore backing = source.detach(js, key);
```

#### Other Operations

```cpp
void otherOps(jsg::Lock& js, jsg::BufferSource& source) {
  v8::Local<v8::Value> handle = source.getHandle(js);

  // Query properties
  size_t size = source.size();
  size_t offset = source.getOffset();
  size_t elemSize = source.getElementSize();
  bool isInt = source.isIntegerType();

  // Get underlying ArrayBuffer size (if not detached)
  KJ_IF_SOME(bufSize, source.underlyingArrayBufferSize(js)) {
    // bufSize is the total ArrayBuffer size, not the view size
  }

  source.trim(js, 10);  // Remove last 10 bytes
  jsg::BufferSource cloned = source.clone(js);   // Shares backing, new JS handle
  jsg::BufferSource copied = source.copy<v8::Uint8Array>(js);  // New backing + handle
  jsg::BufferSource slice = source.getTypedViewSlice<v8::Uint8Array>(js, 0, 100);
  source.setToZero();
}
```

#### GC Visitation

```cpp
class MyClass: public jsg::Object {
private:
  kj::Maybe<jsg::BufferSource> buffer;
  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(b, buffer) {
      visitor.visit(b);
    }
  }
};
```

### V8 Sandbox Considerations

When V8 sandboxing is enabled:

- `BackingStore::from()` copies data if the source is outside the sandbox
- `BackingStore::alloc()` always allocates inside the sandbox
- A `BackingStore` cannot be passed to another isolate unless both are in the same `IsolateGroup`

---

## 22. V8 Platform Wrapper

`V8PlatformWrapper` in `v8-platform-wrapper.h` wraps V8's platform interface to intercept
and customize platform operations. It delegates most operations to an inner `v8::Platform`
but wraps `JobTask` objects. Used internally by `V8System` for monitoring background work,
KJ event loop integration, and debugging/profiling V8's background tasks.

Most users won't interact with it directly.

---

## 23. Module System

JSG supports ES modules (ESM), CommonJS-style modules, and various synthetic module types
(JSON, WASM, data, text). There are two implementations.

### Concepts

**Module Types:**

- `Type::BUNDLE` — Modules from the worker bundle (user code)
- `Type::BUILTIN` — Built-in runtime modules (e.g., `node:buffer`, `cloudflare:sockets`)
- `Type::INTERNAL` — Internal modules only importable by other built-ins

**Resolution Priority:**

1. From bundle code: Bundle -> Builtin -> Fallback
2. From builtin code: Builtin -> Internal
3. From internal code: Internal only

**Module Info Types:** ESM, CommonJsModuleInfo, DataModuleInfo, TextModuleInfo,
WasmModuleInfo, JsonModuleInfo, ObjectModuleInfo, CapnpModuleInfo.

### Original Module Registry (`modules.h`)

```cpp
auto registry = jsg::ModuleRegistryImpl<MyTypeWrapper>::install(isolate, context, observer);

// Add a worker bundle module (ESM)
registry->add(specifier, kj::mv(moduleInfo));

// Add built-in modules
registry->addBuiltinModule("node:buffer", sourceCode, jsg::ModuleRegistry::Type::BUILTIN);
registry->addBuiltinBundle(bundle);  // Add modules from a capnp bundle
registry->addBuiltinModule<MyApiClass>("workerd:my-api");
registry->addBuiltinModule("workerd:instance", kj::mv(myRef));

// Lazy factory
registry->addBuiltinModule("workerd:dynamic",
    [](jsg::Lock& js, auto method, auto& referrer) -> kj::Maybe<ModuleInfo> {
      return ModuleInfo(js, "workerd:dynamic", kj::none,
          ObjectModuleInfo(js, createMyObject(js)));
    });
```

#### Resolving Modules

```cpp
auto* registry = jsg::ModuleRegistry::from(js);
KJ_IF_SOME(info, registry->resolve(js, specifier, referrer)) {
  v8::Local<v8::Module> module = info.module.getHandle(js);
  jsg::instantiateModule(js, module);
  v8::Local<v8::Value> ns = module->GetModuleNamespace();
}
```

#### Synthetic Modules

```cpp
// Data module
auto info = ModuleInfo(js, "data.bin", kj::none, DataModuleInfo(js, buffer));

// Text module
auto info = ModuleInfo(js, "text.txt", kj::none, TextModuleInfo(js, text));

// JSON module
auto info = ModuleInfo(js, "config.json", kj::none, JsonModuleInfo(js, jsonValue));

// WASM module
auto info = ModuleInfo(js, "module.wasm", kj::none, WasmModuleInfo(js, wasmModule));
```

#### CommonJS Modules

```cpp
class MyModuleProvider: public ModuleRegistry::CommonJsModuleInfo::CommonJsModuleProvider {
public:
  JsObject getContext(Lock& js) override {
    return js.obj();  // 'this' context for the module
  }
  JsValue getExports(Lock& js) override {
    return js.obj();  // Initial exports object
  }
};

auto provider = kj::heap<MyModuleProvider>();
auto info = ModuleRegistry::CommonJsModuleInfo(js, "module.js", sourceCode, kj::mv(provider));
```

### New Module Registry (`modules-new.h`)

Key improvements: URL-based specifiers, thread safety across isolate replicas, modular
bundles, import.meta support, import attributes.

#### Creating Modules

```cpp
auto esmModule = Module::newEsm("file:///bundle/worker.js"_url,
    Module::Type::BUNDLE, kj::mv(code),
    Module::Flags::MAIN | Module::Flags::ESM);

auto syntheticModule = Module::newSynthetic("workerd:my-module"_url,
    Module::Type::BUILTIN,
    [](Lock& js, const Url& id, const Module::ModuleNamespace& ns,
       const CompilationObserver& observer) -> bool {
      ns.setDefault(js, js.obj());
      ns.set(js, "foo", js.str("bar"));
      return true;
    },
    kj::arr("foo"_kj));
```

#### Module Handlers

```cpp
auto textHandler = Module::newTextModuleHandler(textContent);
auto dataHandler = Module::newDataModuleHandler(binaryData);
auto jsonHandler = Module::newJsonModuleHandler(jsonContent);
auto wasmHandler = Module::newWasmModuleHandler(wasmBytes);
```

#### Building Module Bundles

```cpp
// Worker bundle
Url bundleBase = "file:///bundle"_url;
ModuleBundle::BundleBuilder bundleBuilder(bundleBase);
bundleBuilder
    .addEsmModule("worker.js", workerSource, Module::Flags::MAIN | Module::Flags::ESM)
    .addEsmModule("utils.js", utilsSource)
    .alias("./lib", "./utils.js");
auto workerBundle = bundleBuilder.finish();

// Builtin bundle
ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN);
builtinBuilder
    .addEsm("node:buffer"_url, bufferSource)
    .addSynthetic("cloudflare:sockets"_url, socketsHandler)
    .addObject<MyApiClass, MyTypeWrapper>("workerd:my-api"_url);
auto builtinBundle = builtinBuilder.finish();

// Internal-only bundle (modules only importable by other built-ins, e.g. node-internal:*)
ModuleBundle::BuiltinBuilder internalBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
internalBuilder
    .addEsm("node-internal:primordials"_url, primordialsSource)
    .addEsm("node-internal:errors"_url, errorsSource);
auto internalBundle = internalBuilder.finish();
```

#### Fallback Bundle

```cpp
auto fallbackBundle = ModuleBundle::newFallbackBundle(
    [](const ResolveContext& context)
        -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
      KJ_IF_SOME(code, fetchModule(context.normalizedSpecifier)) {
        return Module::newEsm(context.normalizedSpecifier.clone(),
                              Module::Type::FALLBACK, kj::mv(code));
      }
      if (shouldRedirect(context.normalizedSpecifier)) {
        return kj::str("node:buffer");  // Redirect
      }
      return kj::none;
    });
```

#### Resolution Context

```cpp
struct ResolveContext {
  Type type;           // BUNDLE, BUILTIN, or BUILTIN_ONLY
  Source source;       // STATIC_IMPORT, DYNAMIC_IMPORT, REQUIRE, or INTERNAL
  const Url& normalizedSpecifier;
  const Url& referrerNormalizedSpecifier;
  kj::Maybe<kj::StringPtr> rawSpecifier;
  kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
};
```

### Module Instantiation

```cpp
v8::Local<v8::Module> module = moduleInfo.module.getHandle(js);
jsg::instantiateModule(js, module);
// Or: jsg::instantiateModule(js, module, InstantiateModuleOptions::NO_TOP_LEVEL_AWAIT);
v8::Local<v8::Value> ns = module->GetModuleNamespace();
```

### Dynamic Import and `require()`

Dynamic imports are handled automatically. For CommonJS compatibility:

```cpp
JsValue exports = ModuleRegistry::requireImpl(js, moduleInfo);
JsValue defaultExport = ModuleRegistry::requireImpl(js, moduleInfo,
    ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT);
```

### Choosing Between Implementations

Use **original** (`modules.h`) when: simpler control, kj::Path specifiers, no cross-replica
sharing needed.

Use **new** (`modules-new.h`) when: URL-based specifiers, import.meta, cross-replica sharing,
import attributes, new code from scratch.

---

## 24. CompileCache

Process-lifetime in-memory cache for V8 compilation data, specifically for built-in JavaScript
modules. Entries are never removed or replaced.

```cpp
const jsg::CompileCache& cache = jsg::CompileCache::get();

KJ_IF_SOME(cachedData, cache.find(cacheKey)) {
  auto v8CachedData = cachedData.AsCachedData();
  // Compile with cached data...
} else {
  // Compile without cache, then store:
  cache.add(cacheKey, std::shared_ptr<v8::ScriptCompiler::CachedData>(
      source.GetCachedData()));
}
```

### `CompileCache::Data`

The cache stores `Data` objects that wrap V8's `ScriptCompiler::CachedData`:

```cpp
class CompileCache::Data {
public:
  // Create V8 cached data for use with ScriptCompiler
  std::unique_ptr<v8::ScriptCompiler::CachedData> AsCachedData();

  const uint8_t* data;  // Raw cached data
  size_t length;        // Data length
};
```

The cache is internally mutex-guarded and safe for concurrent access from multiple threads.

---

["KJ Style Guide"]: https://github.com/capnproto/capnproto/blob/master/style-guide.md
["KJ Tour"]: https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md
[Record]: https://webidl.spec.whatwg.org/#idl-record
[Sequence]: https://webidl.spec.whatwg.org/#idl-sequence
