# What is JSG?

`jsg` is an abstraction API we use to hide many of the complexities of translating back and
forth between JavaScript and C++ types. Think of it as a bridge layer between the kj-based
internals of the runtime and the v8-based JavaScript layer.

Ideally, JSG would be a complete wrapper around V8, such that code using JSG does not need to
interact with V8 APIs at all. Perhaps, then, different implementations of JSG could provide
the same interface on top of different JavaScript engines. However, as of today, this is not
quite the case. At present application code will still need to use V8 APIs directly in some
cases. We would like to improve this in the future.

---

# Part 1: Getting Started

This section covers the prerequisites and foundational concepts you need to understand
before working with JSG.

## The Basics

If you haven't done so already, I recommend reading through both the ["KJ Style Guide"][]
and ["KJ Tour"][] documents before continuing.

Specifically, the ["KJ Style Guide"][] introduces a differentiation between "Value Types" and
"Resource Types":

> There are two kinds of types: values and resources. Value types are simple data
> structures; they serve no purpose except to represent pure data. Resource types
> represent live objects with state and behavior, and often represent resources
> external to the program.

JSG embraces these concepts and offers a C++-to-JavaScript type-mapping layer that is explicitly
built around them.

## jsg::Lock&

In order to execute JavaScript on the current thread, a lock must be acquired on the `v8::Isolate`.
The `jsg::Lock&` represents the current lock. It is passed as an argument to many methods that
require access to the JavaScript isolate and context.

The `jsg::Lock` interface itself provides access to basic JavaScript functionality, such as the
ability to construct basic JavaScript values and call JavaScript functions.

For Resource Types, all methods declared with `JSG_METHOD` and similar macros (described later)
optionally take a `jsg::Lock&` as the first parameter, for instance:

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

Refer to the `jsg.h` header file for more detail on the specific methods available on the `jsg::Lock`.

## V8 System Setup

Before using any JSG functionality, you must initialize V8 by creating a `V8System` instance.
This performs process-wide initialization and should be created once, typically in `main()`.

### `V8System`

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
// Create platform with specific thread count (0 = auto-detect, not recommended)
kj::Own<v8::Platform> platform = jsg::defaultPlatform(4);
```

#### Fatal Error Handling

You can set a custom callback for fatal V8 errors:

```cpp
void myFatalErrorHandler(kj::StringPtr location, kj::StringPtr message) {
  // Log and handle the error
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

When using V8 sandboxing, isolates can be grouped to share a sandbox or isolated for security:

```cpp
// Create a new isolate group (isolated sandbox)
auto group = v8::IsolateGroup::Create();
MyIsolate isolate(v8System, group, config, kj::mv(observer));

// Or use the default group (shared sandbox, less secure but more memory efficient)
MyIsolate isolate(v8System, v8::IsolateGroup::GetDefault(), config, kj::mv(observer));
```

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

---

# Part 2: Core Concepts

This section covers the fundamental type system that JSG provides for mapping between
C++ and JavaScript.

## JSG Value Types

Value Types in JSG include both primitives (e.g. strings, numbers, booleans, etc) and relatively
simple structured types that do nothing but convey data.

At the time of writing this, the primitive value types currently supported by the JSG layer are:

|       C++ Type       |     v8 Type    | JavaScript Type |   Description Notes     |
| -------------------- | -------------- | --------------- | ----------------------- |
| bool                 |  v8::Boolean   |     boolean     |                         |
| double               |  v8::Number    |     number      |                         |
| int                  |  v8::Integer   |     number      |                         |
| int8_t               |  v8::Integer   |     number      |                         |
| int16_t              |  v8::Integer   |     number      |                         |
| int32_t              |  v8::Int32     |     number      |                         |
| int64_t              |  v8::BigInt    |     bigint      |                         |
| uint                 |  v8::Integer   |     number      |                         |
| uint8_t              |  v8::Integer   |     number      |                         |
| uint16_t             |  v8::Integer   |     number      |                         |
| uint32_t             |  v8::Uint32    |     number      |                         |
| uint64_t             |  v8::BigInt    |     bigint      |                         |
| kj::String(Ptr)      |  v8::String    |     string      |                         |
| kj::Date             |  v8::Date      |     Date        |                         |
| nullptr              |  v8::Null      |     null        | See kj::Maybe&lt;T>     |
| nullptr              |  v8::Undefined |     undefined   | See jsg::Optional&lt;T> |

Specifically, for example, when mapping from JavaScript into C++, when JSG encounters a
string value, it can convert that into either a `kj::String`, or `jsg::USVString`,
depending on what is needed by the C++ layer. Likewise, when translating from C++ to
JavaScript, JSG will generate a JavaScript `string` whenever it encounters a `kj::String`,
`kj::StringPtr`, or `jsg::USVString`.

JSG will *not* translate JavaScript `string` to `kj::StringPtr`.

In addition to these primitives, JSG provides a range of additional structured value
types that serve a number of different purposes. We'll explain each individually.

### How does JSG convert values?

V8 provides us with an API for working with JavaScript types from within C++. Unfortunately,
this API can be a bit cumbersome to work with directly. JSG provides an additional
set of mechanisms that wrap the V8 APIs and translate those into more ergonomic C++ types
and structures. Exactly how this mechanism works will be covered later in the advanced
section of this guide.

### Nullable/Optional Types (`kj::Maybe<T>` and `jsg::Optional<T>`)

A nullable type `T` can be either `null` or `T`, where `T` can be any Value or Resource
type (we'll get into that a bit a later). This is represented in JSG using `kj::Maybe<T>`.

An optional type `T` can be either `undefined` or `T`, again, where `T` can be any Value
or Resource type. This is represented in JSG using `jsg::Optional<T>`.

For example, if at the C++ layer I have the type `kj::Maybe<kj::String>`, translating
from JavaScript I can pass one of either a `null`, `undefined`, or any value that can
be coerced into a JavaScript string. If I have the type `jsg::Optional<kj::String>`,
I can pass either `undefined` or any value that can be coerced into a JavaScript
string.

Take careful note of the differences there: `kj::Maybe<kj::String>` allows `null` or
`undefined`, `jsg::Optional<kj::String>` only allows `undefined`.

At the C++ layer, `kj::Maybe<T>` and `jsg::Optional<T>` are semantically equivalent.
Their value is one of either `kj::none` or type `T`. When an empty `kj::Maybe<T>` is
passed out to JavaScript, it is mapped to `null`. When an empty `jsg::Optional<T>` is
passed out to JavaScript, it is mapped to `undefined`.

This mapping, for instance, means that JavaScript `undefined` can be translated to
`kj::Maybe<T>`, but `kj::Maybe<T>` passed back out to JavaScript will translate to
JavaScript `null`.

To help illustrate the differences, the following illustrates the mapping when
translating from a JavaScript value of `undefined` or `null`.

| Type                                 | JavaScript Value | C++ value   |
| ------------------------------------ | ---------------- | ----------- |
| `kj::Maybe<kj::String>`              | `undefined`      | `kj::none`  |
| `kj::Maybe<kj::String>`              | `null`           | `kj::none`  |
| `jsg::Optional<kj::String>`          | `undefined`      | `kj::none`  |
| `jsg::Optional<kj::String>`          | `null`           | `{throws}`  |
| `jsg::LenientOptional<kj::String>`*  | `null`           | `kj::none`  |

`*` `jsg::LenientOptional` is discussed a bit later.

### Union Types (`kj::OneOf<T...>`)

A union type `<T1, T2, ...>` is one whose value is one of the types listed. It is
represented in JSG using `kj::OneOf<T...>`.

For example, `kj::OneOf<kj::String, uint16_t, kj::Maybe<bool>>` can be either:
a) a string, b) a 16-bit integer, c) `null`, or (d) `true` or `false`.

**Important:** JSG validates union types at compile-time according to Web IDL rules. This means:

- A union can have at most one nullable type (`kj::Maybe<T>`) or dictionary type
- A union can have at most one numeric type, one string type, one boolean type, etc.
- Multiple interface types (JSG_RESOURCE types) are allowed as long as they are distinct

For example, `kj::OneOf<int, double>` will produce a compile error because both are numeric
types. See the "Web IDL Type Mapping" section for the complete list of validation rules.

When a value could match multiple types in a union, JSG attempts to match types in a
specific order based on their Web IDL category. Interface types are checked first, then
dictionaries, then sequences, then primitives.

### Array types (`kj::Array<T>` and `kj::ArrayPtr<T>`)

The `kj::Array<T>` and `kj::ArrayPtr<T>` types map to JavaScript arrays. Here, `T` can
be any value or resource type. The types `kj::Array<kj::byte>` and `kj::ArrayPtr<kj::byte>`
are handled differently.

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

### Set type (`kj::HashSet<T>`)

The `kj::HashSet<T>` type maps to JavaScript sets. The `T` can be any value, though there are
currently some restrictions. For example, you cannot have a `kj::HashSet<CustomStruct>`.

```cpp
void doSomething(kj::HashSet<kj::String> strings) {
  KJ_DBG(strings.has("a"));
}
```

```js
doSomething(new Set(['a', 'b', 'c']));
```

### Sequence types (`jsg::Sequence<T>`)

A [Sequence][] is any JavaScript object that implements `Symbol.iterator`. These are
handled in JSG using the `jsg::Sequence<T>` type. At the c++ level, a `jsg::Sequence<T>`
is semantically identical to `kj::Array<T>` but JSG will handle the mapping to and
from JavaScript a bit differently.

With `kj::Array<T>`, the input from JavaScript *must* always be a JavaScript array.

With `jsg::Sequence<T>`, the input from JavaScript can be any object implementing
`Symbol.iterator`.

When a `jsg::Sequence<T>` is passed back out to JavaScript, however, JSG will always
produce a JavaScript array.

```cpp
void doSomething(jsg::Sequence<kj::String> strings) {
  KJ_DBG(strings[0]);  // a
  KJ_DBG(strings[1]);  // b
  KJ_DBG(strings[2]);  // c
}
```

```js
doSomething({
  *[Symbol.iterator] () {
    yield 'a';
    yield 'b';
    yield 'c';
  }
});

// or

doSomething(['a', 'b', 'c']);
```

#### What does it mean to implement `Symbol.iterator`?

`Symbol.iterator` is a standard EcmaScript language feature for creating iterable objects
using the generator pattern.

For instance, in the example here,

```js
const iterableObject = {
  [Symbol.iterator]* () {
    yield 'a';
    yield 'b';
    yield 'c';
  }
};

const iter = iterableObject[Symbol.iterator]();
```

The `iter` will be an instance of a Generator object.

Generator objects expose three basic methods: `next()`, `return()`, and `throw()`.
We'll only concern ourselves with `next()` right now.

The first time `next()` is called on the generator object, the function body of the
`Symbol.iterator` function on `iterableObject` will be invoked up to the first `yield`
statement. The value yielded will be returned in an object:

```js
const result = iter.next();
console.log(result.done);  // false!
console.log(result.value); // 'a'
```

Each subsequent call to `next()` will advance the execution of the generator function
to the next `yield` or to the end of the function.

```js
const result = iter.next();
console.log(result.done);  // false!
console.log(result.value); // 'b'

const result = iter.next();
console.log(result.done);  // false!
console.log(result.value); // 'c'

const result = iter.next();
console.log(result.done);  // true!
console.log(result.value); // undefined
```

Managing the state of these can be fairly complicated. The `jsg::Sequence<T>` and
`jsg::Generator<T>` (discussed next) manage the complexity for us, making it easy to
consume the iterator data either as an array or as a sequence of simplified callbacks.

### Generator types (`jsg::Generator<T>` and `jsg::AsyncGenerator<T>`)

`jsg::Generator<T>` provides an alternative to `jsg::Sequence` to handling objects that
implement `Symbol.iterator`. Whereas `jsg::Sequence` will always produce a complete
copy of the iterable sequence that is equivalent to `kj::Array`, the `jsg::Generator<T>`
provides an API for iterating over each item one at a time.

```cpp
void doSomething(jsg::Generator<kj::String> strings) {
  // Called for each item produced by the generator.
  strings.forEach([](jsg::Lock& js, kj::String value, jsg::GeneratorContext<kj::String> ctx) {
    KJ_DBG(value);
  });
}
```

```js
doSomething({
  *[Symbol.iterator] () {
    yield 'a';
    yield 'b';
    yield 'c';
  }
});

// or

doSomething(['a', 'b', 'c']);
```

For `jsg::Generator<T>`, iteration over the types is fully synchronous. To support asynchronous
generators and async iteration, JSG provides `jsg::AsyncGenerator<T>`. The C++ API for
async iteration is nearly identical to that of `jsg::Generator<T>`.

### Record/Dictionary types (`jsg::Dict<T>`)

A [Record][] type is an ordered set of key-value pairs. They are represented in JSG using the
`jsg::Dict<T>` and in JavaScript as ordinary JavaScript objects whose string-keys all map to the
same type of value. For instance, given `jsg::Dict<bool>`, a matching JavaScript object would be:

```js
{
  "abc": true,
  "xyz": false,
  "foo": true,
}
```

Importantly, the keys of a Record are *always* strings and the values are *always* the same type.

### Non-coercible and Lenient Types (`jsg::NonCoercible<T>` and `jsg::LenientOptional<T>`)

By default, JSG supports automatic coercion between JavaScript types where possible. For instance,
when a `kj::String` is expected on the C++ side, any JavaScript value that can be coerced into a
string can be passed. JSG will take care of converting that value into a string. Because values
like `null` and `undefined` can be coerced into the strings `'null'` and `'undefined'`, automatic
coercion is not always desired.

JSG provides the `jsg::NonCoercible<T>` type as a way of declaring that you want a type `T` but
do not want to apply automatic coercion. For example, `jsg::NonCoercible<kj::String>` will *only*
accept string values.

At the time of this writing, the only values of `T` supported by `jsg::NonCoercible` are `kj::String`,
`bool`, and `double`.

Also by default, given a `jsg::Optional<T>`, JSG will throw a type error if the JavaScript value
given cannot be interpreted as `T`. For instance, if I have `jsg::Optional<jsg::Dict<bool>>` and
a JavaScript string is passed, JSG will throw a type error. The `jsg::LenientOptional<T>` provides
an alternative that will instead ignore incorrect values and pass them on a `undefined` instead of
throwing a type error.

### TypedArrays (`kj::Array<kj::byte>` and `jsg::BufferSource`)

In V8, `TypedArray`s (e.g. `Uint8Array`, `Int16Array`, etc) and `ArrayBuffer`s are backed by a
`v8::BackingStore` instance that owns the actual memory storage. JSG provides a couple of type
mappings for these structures.

One choice is to map to and from a `kj::Array<kj::byte>`.

When receiving a `kj::Array<kj::byte>` in C++ *from* a JavaScript `TypedArray` or `ArrayBuffer`,
it is important to understand that the underlying data is not copied or transferred. Instead, the
`kj::Array<kj::byte>` provides a *view* over the same underlying `v8::BackingStore` as the
`TypedArray` or `ArrayBuffer`.

```
                   +------------------+
                   | v8::BackingStore |
                   +------------------+
                     /             \
   +-----------------+             +---------------------+
   | v8::ArrayBuffer | ----------> | kj::Array<kj::byte> |
   +-----------------+             +---------------------+
```

Because both the `v8::ArrayBuffer` and `kj::Array<kj::byte>` are *mutable*, changes in the data
in one are immediately readable by the other (because they share the exact same memory). Importantly,
this is true no matter what kind of `TypedArray` it is so care must be taken if the `TypedArray`
uses multi-byte values (e.g. a `Uint32Array` would still be mapped to `kj::Array<kj::byte>` despite
the fact that each member entry is 4-bytes.)

When a `kj::Array<kj::byte>` is passed back *out* to JavaScript, it is always mapped into a *new*
`ArrayBuffer` instance over the same memory (no copy of the data is made but ownership of the
memory is transferred to the `std::shared_ptr<v8::BackingStore>` instance that underlies the
`ArrayBuffer`).

Note that this mapping behavior can get a bit hairy if a single backing store is passed back and
forth across the JS/C++ boundary multiple times. Specifically, if we pass an `ArrayBuffer` from
JS to C++, the `std::shared_ptr<v8::BackingStore>` holding the underlying data will be extracted
and will be attached to a `kj::Array<kj::byte>` in C++. If that `kj::Array<kj::byte>` is passed
back out to JavaScript, a *new* `std::shared_ptr<v8::BackingStore>` will be created that has a
disposer that wraps and owns the `kj::Array<kj::byte>`. That `v8::BackingStore` will be attached
to a *new* `ArrayBuffer` passed out to JavaScript. If that `ArrayBuffer` is passed *back* into
C++, then the wrapping process will repeat.

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

While the structures all point at the same underlying memory allocation, we end up with multiple
nested levels of `v8::BackingStore` and `kj::Array<kj::byte>` instances.

For many cases, this mapping behavior is just fine, but some APIs (such as Streams) require a
more nuanced type of mapping. For those cases, we provide `jsg::BufferSource`.

A `jsg::BufferSource` wraps the v8 handle of a given `TypedArray` or `ArrayBuffer` and remembers
its type. When the `jsg::BufferSource` is passed back out to JavaScript, it will map to exactly
the same kind of object that was passed in (e.g. `Uint16Array` passed to `jsg::BufferSource` will
produce a `Uint16Array` when passed back out to JavaScript.) The same
`std::shared_ptr<v8::BackingStore>` will be maintained across the JS/C++ boundary.

The `jsg::BufferSource` also supports the ability to "detach" the backing store. What this does
is separate the `v8::BackingStore` from the original `TypedArray`/`BufferSource` such that the
original can no longer be used to read or mutate the data. This is important in cases where ownership
of the data storage must transfer and cannot be shared.

### Functions (`jsg::Function<Ret(Args...)>`)

The `jsg::Function<Ret(Args...)>` type provides a wrapper around JavaScript functions, making it
easy to call such functions from C++ as well as store references to them.

There are generally three ways of using `jsg::Function`:

* Taking a JS Function and making it available to easily invoke from C++.
* Taking a C++ lambda function and wrapping it in a JavaScript function.
* Making it possible to call a function in C++ without needing to know if it is JavaScript or C++.

For the first item, imagine the following case:

We have a C++ function that takes a callback argument. The callback argument takes an `int`
as an argument (Ignore the `jsg::Lock& js` part for now, we'll explain that in a bit). As soon
as we get the callback, we invoke it as a function.

```cpp
void doSomething(jsg::Lock& js, jsg::Function<void(int)> callback) {
  callback(js, 1);
}
```

On the JavaScript side, we would have:

```js
doSomething((arg) => { console.log(arg); });  // prints "1"
```

For the second bullet (taking a C++ lambda function and wrapping it), we can go the other way:

```cpp
jsg::Function<void(int)> getFunction() {
  return jsg::Function<void(int)>([](jsg::Lock& js, int val) {
    KJ_DBG(val);
  });
}
```

```js
const func = getFunction();
func(1);  // prints 1
```

### Promises (`jsg::Promise<T>`)

The `jsg::Promise<T>` wraps a JavaScript promise with a syntax that makes it more
natural and ergonomic to consume within C++.

#### Creating Promises

Use `jsg::Lock` to create promises:

```cpp
// Create an already-resolved promise
jsg::Promise<int> resolved = js.resolvedPromise(42);
jsg::Promise<void> resolvedVoid = js.resolvedPromise();

// Create a rejected promise
jsg::Promise<int> rejected = js.rejectedPromise<int>(js.error("Something went wrong"));

// Create a promise with a resolver for later fulfillment
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
  // Called when the promise resolves successfully
  return value * 2;  // Returns jsg::Promise<int>
}, [](jsg::Lock& js, jsg::Value exception) {
  // Called when the promise rejects
  return 0;  // Must return the same type as the success handler
});

// Chain with only a success handler (errors propagate)
promise.then(js, [](jsg::Lock& js, int value) {
  return kj::str("Value: ", value);  // Returns jsg::Promise<kj::String>
});

// Chain with only an error handler
promise.catch_(js, [](jsg::Lock& js, jsg::Value exception) {
  // Handle error, must return the promise's type
  return 0;
});
```

**Important:** Both handlers passed to `.then()` must return exactly the same type.

#### `Promise<void>`

For promises that don't carry a value:

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
    // Start async work...
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

#### `.whenResolved()`

Unlike `.then()`, `whenResolved()` doesn't consume the promise and can be called multiple
times to create branches:

```cpp
jsg::Promise<int> promise = // ...

// Create a Promise<void> that resolves when the original resolves
jsg::Promise<void> done = promise.whenResolved(js);
```

#### Consuming the Handle

To pass a promise to V8 APIs or return it directly:

```cpp
v8::Local<v8::Promise> handle = promise.consumeHandle(js);
```

After calling `consumeHandle()`, the `jsg::Promise` is consumed and cannot be used again.

#### Returning Promises from Methods

Methods exposed to JavaScript can return promises:

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

The `jsg::Name` type is a wrapper around values that can be used as property names in
JavaScript (specifically, strings and symbols). It is used when an API needs to accept
either and treat them the same.

```cpp
jsg::Name doSomething(jsg::Name name) {
  // The name can be stored and used as a hashmap key, etc.
  // When returned back to JavaScript, it will be the same type as was passed in.
  return kj::mv(name);
}
```

### Reference types (`jsg::V8Ref<T>`, `jsg::Value`, `jsg::Ref<T>`)

#### `jsg::V8Ref<T>` (and `jsg::Value`, and `jsg::HashableV8Ref<T>`)

The `jsg::V8Ref<T>` type holds a persistent reference to a JavaScript type. The `T` must
be one of the `v8` types (e.g. `v8::Object`, `v8::Function`, etc). A `jsg::V8Ref<T>` is used
when storing references to JavaScript values that are not being translated to corresponding C++
types.

Importantly, a `jsg::V8Ref<T>` maintains a *strong* reference to the JavaScript type, keeping
it from being garbage collected. (We'll discuss garbage collection considerations a bit later.)

The `jsg::Value` type is a typedef alias for `jsg::V8Ref<v8::Value>`, a generic type that can
hold any JavaScript value.

`jsg::V8Ref<T>` are reference counted. Multiple instances can share references to the same
underlying JavaScript value. When the last instance is destroyed, the underlying value is freed
to be garbage collected.

```cpp
jsg::Lock& js = // ...

jsg::V8Ref<v8::Boolean> boolRef1 = js.v8Ref(v8::True(js.v8Isolate));

jsg::V8Ref<v8::Boolean> boolRef2 = boolRef1.addRef(js);

KJ_DBG(boolRef1 == boolRef2);  // prints "true"

// Getting the v8::Local<T> from the V8Ref requires a v8::HandleScope to be
// on the stack. We provide a convenience method on jsg::Lock to ensure that:
v8::Local<v8::Boolean> boolLocal = js.withinHandleScope([&] { return boolRef1.getHandle(js); });
```

The `jsg::HashableV8Ref<T>` type is a subclass of `jsg::V8Ref<T>` that also implements
`hashCode()` for use as a key in a `kj::HashTable`.

#### `jsg::Ref<T>`

The `jsg::Ref<T>` type holds a persistent reference to a JSG Resource Type. Resource Types
will be covered in more detail later so for now it's enough just to cover a couple of the
basics.

A `jsg::Ref<T>` holds a *strong* reference to the resource type, keeping it from being garbage
collected (Garbage collection and GC visitation is discussed [later](#garbage-collection-and-gc-visitation)).

`jsg::Ref<T>` are reference counted. Multiple instances can share references to the same
underlying resource type. When the last instance is destroyed, the underlying value is freed
to be garbage collected.

Importantly, a resource type is a C++ object that *may* have a corresponding JavaScript "wrapper"
object. This wrapper is created lazily when the `jsg::Ref<T>` instance is first passed back
out to JavaScript via JSG mechanisms.

```cpp
// We'll discuss js.alloc a bit later as part of the introduction to Resource Types.
jsg::Ref<Foo> foo = js.alloc<Foo>();

jsg::Ref<Foo> foo2 = foo.addRef();

js.withinHandleScope([&] {
  KJ_IF_SOME(handle, foo.tryGetHandle(js.v8Isolate)) {
    // handle is the Foo instance's JavaScript wrapper.
  }
});
```

### Memoized and Identified types

#### `jsg::MemoizedIdentity<T>`

Typically, whenever a C++ type that is *not* a Resource Type is passed out to JavaScript, it is
translated into a *new* JavaScript value. For instance, in the following example, the `Foo` struct
that is returned from `getFoo()` is translated into a new JavaScript object each time.

```cpp
struct Foo {
  int value;
  JSG_STRUCT(value);
};

Foo echoFoo(Foo foo) {
  return foo;
}
```

Then in JavaScript:

```js
const foo = { value: 1 };
const foo2 = echoFoo(foo);
foo !== foo2;  // true... they are different objects
```

The `jsg::MemoizedIdentity<T>` type is a wrapper around a C++ type that allows the JavaScript
value to be preserved such that it can be passed multiple times to JavaScript:

```cpp
struct Foo {
  int value;
  // We'll discuss JSG_STRUCT a bit later.
  JSG_STRUCT(value);
};

jsg::MemoizedIdentity<Foo> echoFoo(jsg::MemoizedIdentity<Foo> foo) {
  return kj::mv(foo);
}
```

Then in JavaScript:

```js
const foo = { value: 1 };
const foo2 = echoFoo(foo);
foo === foo2;  // true... they are the same object
```

#### `jsg::Identified<T>`

The `jsg::Identified<T>` type is a wrapper around a C++ type that captures the identity of the
JavaScript object that was passed. This is useful, for instance, if you need to be able to recognize
when the application passes the same value later.

```cpp
struct Foo {
  int value;
  JSG_STRUCT(value);
};

void doSomething(jsg::Identified<Foo> obj) {
  auto identity = obj.identity; // HashableV8Ref<v8::Object>
  Foo foo = obj.unwrapped;
}
```

### Using `JSG_STRUCT` to declare structured value types

In JSG, a "struct" is a JavaScript object that can be mapped to a C++ struct.

For example, the following C++ struct can be mapped to a JavaScript object that
contains three properties: `'abc'`, `'xyz'`, and `'val'`:

```cpp
struct Foo {
  kj::String abc;
  jsg::Optional<bool> xyz;
  jsg::Value val;

  JSG_STRUCT(abc, xyz, val);

  int onlyInternal = 1;
};
```

The `JSG_STRUCT` macro adds the necessary boilerplate to allow JSG to perform
the necessary mapping between the C++ struct and the JavaScript object. Only the
properties listed in the macro are mapped. (In this case, `onlyInternal` is not
included in the JavaScript object.)

If the struct has a validate() method, it is called when the struct is unwrapped from v8.
This is an opportunity for it to throw a TypeError based on some custom logic.
The signature for this method is `void validate(jsg::Lock&);`

```cpp
struct ValidatingFoo {
  kj::String abc;

  void validate(jsg::Lock& lock) {
    JSG_REQUIRE(abc.size() != 0, TypeError, "Field 'abc' had no length in 'ValidatingFoo'.");
  }

  JSG_STRUCT(abc);
};
```

In this example the validate method would throw a `TypeError` if the size of the `abc` field was zero.

```cpp
Foo someFunction(Foo foo) {
  KJ_DBG(foo.abc);  // a
  KJ_IF_SOME(xyz, foo.xyz) {
    KJ_DBG(xyz);  // true
  }
  KJ_DBG(val); // [object Object]
  KJ_DBG(onlyInternal); 1

  // Move semantics are required because kj::String and jsg::Value both
  // require move...
  return kj::mv(foo);
}
```

Then in JavaScript:

```js
const foo = someFunction({
  abc: 'a',
  xyz: true,
  val: { },

  // Completely ignored by the mapping
  ignored: true,
});

console.log(foo.abc);  // a
console.log(foo.xyz);  // true
console.log(foo.val);  // { }
console.log(onlyInternal);  // undefined
```

When passing an instance of `Foo` from JavaScript into C++, any additional properties
on the object are ignored.

To be used, JSG structs must be declared as part of the type system. We'll describe how
to do so later when discussing how JSG is configured and initialized.

## Resource Types

A Resource Type is a C++ type that is mapped to a JavaScript object that is *not* a plain
JavaScript object. Or, put another way, it is a JavaScript object that is backed by a
corresponding C++ object that provides the implementation.

There are three key components of a Resource Type:

* The underlying C++ type,
* The JavaScript wrapper object, and
* The `v8::FunctionTemplate`/`v8::ObjectTemplate` that define how the two are connected.

For example, suppose we have the following C++ type (we'll introduce `jsg::Object` and the
other `JSG*` pieces in a bit):

```cpp
class Foo: public jsg::Object {
public:
  Foo(int value): value(value) {}

  static jsg::Ref<Foo> constructor(int value);

  int getValue();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(value, getValue);
  }

private:
  int value;
};
```

This corresponds to a JavaScript object that has a constructor and a single read-only
property:

```js
const foo = new Foo(123);
console.log(foo.value);  // 123
```

The JSG macros and other pieces transparently handle the necessary plumbing to connect
the C++ class to the JavaScript implementation of it such that the constructor (e.g. `new Foo(123)`)
is automatically routed to the static `Foo::constructor()` method, and the getter accessor for
the `value` property is automatically routed to the `Foo::getValue()` method.

JSG intentionally hides away most of the details of how this works so we're not going to cover
that piece here.

The example illustrates all of the top-level pieces of defining a Resource Type but there are
a range of details. We'll cover those next.

### `jsg::Object`

All Resource Types must inherit from `jsg::Object`. This provides the necessary plumbing for
everything to work. The `jsg::Object` class must be publicly inherited but it does not add
any additional methods or properties that need to be directly used (it does add some but those
are intended to be used via other JSG APIs).

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

For a Resource Type to be constructible from JavaScript, it must have a static method named
`constructor()` that returns a `jsg::Ref<T>` where `T` is the Resource Type. If this static
method is not provided, attempts to create new instances using `new ...` will fail with an
error.

```cpp
class Foo: public jsg::Object {
public:
  Foo(int value): value(value) {}

  static jsg::Ref<Foo> constructor(jsg::Lock& js, int value) {
    return js.alloc<Foo>(value);
  }

  JSG_RESOURCE_TYPE(Foo) {}

private:
  int value;
};
```

### `JSG_RESOURCE_TYPE(T)`

All Resource Types must contain the `JSG_RESOURCE_TYPE(T)` macro. This provides the necessary
declarations that JSG uses to construct the `v8::FunctionTemplate` and `v8::ObjectTemplate` that
underlying the automatic mapping to JavaScript.

The `JSG_RESOURCE_TYPE(T)` is declared as a block, for instance:

```cpp
JSG_RESOURCE_TYPE(Foo) {
  // ...
}
```

Within the block are a series of zero or more additional `JSG*` macros that define properties,
methods, constants, etc. on the Resource Type.

#### `JSG_METHOD(name)` and `JSG_METHOD_NAMED(name, method)`

Used to declare that the given method should be callable from JavaScript on instances of the
resource type.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  void bar();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(bar);
  }
}
```

```js
const foo = new Foo();
foo.bar();
```

The method on the `Foo` instance is the same as the method on the C++ class. Sometimes, however,
it is necessary to define a different name (such as when the name you want exposed in JavaScript
is a C++ keyword). For that case, `JSG_METHOD_NAMED` can be used:

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  void delete_();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD_NAMED(delete, delete_);
  }
}
```

```js
const foo = new Foo();
foo.delete();
```

Importantly, the method in JavaScript is exposed on the prototype of the object instance so it is
possible to override the method in a subclass.

```js
class MyFoo extends Foo {
  delete() {
    // Calls the actual delete_() method on the C++ object.
    super.delete();
  }
}
```

Arguments accepted by the functions, and the return values, are automatically marshalled to and from
JavaScript using the mapping rules described previously.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor(kj::String abc);

  kj::String bar(int x, kj::String y);

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(bar);
  }
```

```js
const foo = new Foo('hello');
const result = foo.bar(123, 'there');
```

#### `JSG_STATIC_METHOD(name)` and `JSG_STATIC_METHOD_NAMED(name, method)`

Used to declare that the given method should be callable from JavaScript on the class for the resource type.

```cpp
class Foo: public jsg::Object {
public:
  static void bar();
  static void delete_();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_STATIC_METHOD(bar);
    JSG_STATIC_METHOD_NAMED(delete, delete_);
  }
}
```

```js
Foo.bar();
Foo.delete();
```

#### Properties

Properties on JavaScript objects are fairly complicated. There are a number of different types.

##### Prototype vs. Instance Properties

A Prototype property is one that is defined on the prototype of the JavaScript object. It is typically
implemented as a getter (readonly) or getter/setter (readwrite) pair. The property definition is shared
by all instances of the object and can be overridden by subclasses (the value is still specific to the
individual instance).

An Instance property is one that is defined specifically as an own property of an individual instance.
It can be implemented as a getter (readonly) or getter/setter (readwrite) pair. The property definition
is specific to the individual instance and cannot be overridden by subclasses.

##### Read-only vs. Read-write

A read-only property is one that can only be read from JavaScript. It is implemented as a getter.

A read-write property is one that can be read from and written to from JavaScript. It is implemented as
a getter/setter pair.

##### `JSG_READONLY_PROTOTYPE_PROPERTY(name, getter)` and `JSG_PROTOTYPE_PROPERTY(name, getter, setter)`

These define readonly and read/write prototype properties on the Resource Type.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  kj::String getAbc();
  int getXyz();
  void setXyz(int value);

  JSG_RESOURCE_TYPE(Foo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(abc, getAbc);
    JSG_PROTOTYPE_PROPERTY(xyz, getXyz, setXyz);
  }
};
```

```js
const foo = new Foo();
// abc is read-only
foo.abc; // 'hello'

// xyz is read-write
foo.xyz; // 123
foo.xyz = 456;
```

Importantly, because these are prototype properties, they can be overridden in subclasses.

```js
class MyFoo extends Foo {
  get abc() {
    return 'goodbye';
  }
  set xyz(value) {
    // Calls the actual setXyz() method on the C++ object.
    super.xyz = value;
  }
}
```

##### `JSG_READONLY_INSTANCE_PROPERTY(name, getter)` and `JSG_INSTANCE_PROPERTY(name, getter, setter)`

These define readonly and read/write instance properties on the Resource Type.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  kj::String getAbc();
  int getXyz();
  void setXyz(int value);

  JSG_RESOURCE_TYPE(Foo) {
    JSG_READONLY_INSTANCE_PROPERTY(abc, getAbc);
    JSG_INSTANCE_PROPERTY(xyz, getXyz, setXyz);
  }
};
```

```js
const foo = new Foo();
// abc is read-only
foo.abc; // 'hello'

// xyz is read-write
foo.xyz; // 123
foo.xyz = 456;
```

While these *appear* the same as the prototype properties, it is important to note that they are
defined on the `this` object and not the prototype. This means that they cannot be overridden in subclasses.

```js
class MyFoo extends Foo {
  get abc() {
    // This is not called because the override is defined on the prototype and the
    // abc property is a an own property of the instance.
    return 'goodbye';
  }
  set xyz(value) {
    // This is not called because the override is defined on the prototype and the
    // abc property is a an own property of the instance.
    super.xyz = value;
  }
}
```

##### `JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getter)` and `JSG_LAZY_INSTANCE_PROPERTY(name, getter)`

A lazy instance property is one that is only evaluated once and then cached. This is useful for cases
where a default value is provided but there's no reason to reevaluate it or we want the property to be
easily overridable by users. This is typically only used to introduce new global properties when we
don't want to risk breaking user code.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  kj::String getAbc();
  int getXyz();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(abc, getAbc);
    JSG_LAZY_INSTANCE_PROPERTY(xyz, getXyz);
  }
};
```

```js
const foo = new Foo();
// abc is read-only
foo.abc; // 'hello'
foo.abc = 1; // ignored
foo.abc;  // 'hello'

// xyz is read-write, but there is no setter necessary.
foo.xyz; // 123
foo.xyz = "hello"; // The value type is also not enforced.
foo.xyz; // 'hello'
```

Because these are instance properties, they cannot be overridden in subclasses.

#### Static Constants

Static constants are a special read-only property that is defined on the class itself.

```cpp
class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  static const int ABC = 123;

  JSG_RESOURCE_TYPE(Foo) {
    JSG_STATIC_CONSTANT(ABC);
  }
};
```

```js
Foo.ABC; // 123
```

#### Iterable and Async Iterable Objects

Iterable and Async Iterable objects are a special type of object that can be used in `for...of` loops.
They are implemented as a JavaScript class that implements the `Symbol.iterator` and/or
`Symbol.asyncIterator` methods.

These are implemented using either the `JSG_ITERABLE` or `JSG_ASYNC_ITERABLE` macros with help from
the `JSG_ITERATOR` and `JSG_ASYNC_ITERATOR` macros.

```cpp
class Foo: public jsg::Object {
private:
  using EntryIteratorType = kj::Array<kj::String>;

  struct IteratorState final {
    // ...
    void visitForGc(jsg::GcVisitor& visitor) {
      // ...
    }
  };

public:
  static jsg::Ref<Foo> constructor();

  // JSG_ITERATOR will be described in more detail later. It is used to define the
  // actual implementation of the iterator.
  JSG_ITERATOR(Iterator, entries,
               EntryIteratorType,
               IteratorState,
               iteratorNext);

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(entries);
    JSG_ITERABLE(entries);
  }
};
```

```js
const foo = new Foo();
for (const entry of foo) {
  // Each entry is a string provided by the entries() method.
}
```

#### Inheriting from other Resource Types

The `JSG_INHERIT` macro is used to declare that a Resource Type inherits from another:

```cpp
class Bar: public jsg::Object {
public:
  static jsg::Ref<Bar> constructor();

  JSG_RESOURCE_TYPE(Bar) {}
};

class Foo: public Bar {
public:
  static jsg::Ref<Foo> constructor();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_INHERIT(Bar);
  }
};
```

```js
const foo = new Foo();
foo instanceof Bar; // true
```

#### Exposing nested Resource Types

There are times where it is useful to expose the constructor of a nested Resource Type. We use this
mechanism, for instance, to expose new constructors on the global scope.

```cpp
class Bar: public jsg::Object {
public:
  static jsg::Ref<Bar> constructor();

  JSG_RESOURCE_TYPE(Bar) {}
};

class OtherBar: public jsg::Object {
public:
  static jsg::Ref<OtherBar> constructor();

  JSG_RESOURCE_TYPE(OtherBar) {}
};

class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

  JSG_RESOURCE_TYPE(Foo) {
    JSG_NESTED_TYPE(Bar);
    JSG_NESTED_TYPE_NAMED(OtherBar, Baz);
  }
};
```

```js
const foo = new Foo();
const bar = new Foo.Bar();
const baz = new Foo.Baz();
```

#### Callable objects (`JSG_CALLABLE()`)

The `JSG_CALLABLE(name)` macro allows a Resource Type to be called as a function. For instance,
given the following resource type:

```cpp
class MyAssert: public jsg::Object {
public:
  static jsg::Ref<MyAssert> constructor();

  void ok(boolean condition) {
    JSG_REQUIRE(condition, Error, "Condition failed!");
  }

  JSG_RESOURCE_TYPE(MyAssert) {
    JSG_CALLABLE(ok);
    JSG_METHOD(ok);
  }
};
```

The assertion test can be used in JavaScript as:

```js
const assert = new MyAssert();

assert.ok(true); // No error

assert(true);  // error
```

#### Additional configuration and compatibility flags

The `JSG_RESOURCE_TYPE` macro can accept an optional second parameter: a
`CompatibilityFlags::Reader` that allows you to conditionally expose different APIs based on
the worker's compatibility date and flags.

```cpp
class MyApi: public jsg::Object {
public:
  kj::String oldMethod() { return kj::str("old"); }
  kj::String newMethod() { return kj::str("new"); }

  JSG_RESOURCE_TYPE(MyApi, workerd::CompatibilityFlags::Reader flags) {
    // Always expose the old method
    JSG_METHOD(oldMethod);

    // Only expose the new method when the feature flag is enabled
    if (flags.getMyNewFeature()) {
      JSG_METHOD(newMethod);
    }
  }
};
```

This pattern is used extensively in workerd to maintain backward compatibility while gradually
rolling out new APIs and behaviors. The compatibility flags are defined in
`src/workerd/io/compatibility-date.capnp`.

Common use cases:
- Adding new methods or properties that shouldn't be available to older workers
- Changing behavior of existing methods based on compatibility settings
- Enabling experimental features behind flags

## Declaring the Type System (advanced)

For all of the JSG type system to function, we need to declare the types that we want to include.
Without diving into all of the internal details that make it happen, for now we'll focus on just
the top level requirements.

Somewhere in your application (e.g. see server/workerd-api.c++) you must include an instance of
the `JSG_DECLARE_ISOLATE_TYPE` macro. This macro sets up the base isolate types and helpers that
implement the entire type system.

```cpp
JSG_DECLARE_ISOLATE_TYPE(MyIsolate, api::Foo, api::Bar)
```

In this example, we define a `MyIsolate` base that includes `api::Foo` and `api::Bar` as
supported resource types. Every resource type and JSG_STRUCT type that you wish to use must be
listed in the `JSG_DECLARE_ISOLATE_TYPE` definition.

In our code, you will see a pattern like:

```cpp
JSG_DECLARE_ISOLATE_TYPE(JsgSserveIsolate,
  EW_GLOBAL_SCOPE_ISOLATE_TYPES,
  EW_ACTOR_ISOLATE_TYPES,
  EW_ACTOR_STATE_ISOLATE_TYPES,
  EW_ANALYTICS_ENGINE_ISOLATE_TYPES,
  ...
```

Each of those `EW_*_ISOLATE_TYPES` macros are defined in their respective header files and act
as a shortcut to make managing the list easier.

The `JSG_DECLARE_ISOLATE_TYPE` macro should only be defined once in your application.

---

# Part 3: Memory Management

This section covers garbage collection, GC visitation, memory tracking, and the
mechanisms JSG provides for safe memory management across the C++/JavaScript boundary.

## Garbage Collection and GC Visitation

Garbage Collection can be a tricky topic to understand. We're not going to delve into the details here.
Instead, we'll focus on the basics of how to implement GC visitation for your types and identify a few
of the more important things to keep in mind.

V8's garbage collector is a mark-and-sweep collector. It works by first marking all of the objects
that are reachable from the root set. Then, it sweeps through the heap and frees all of the objects
that were not marked. "Marking" here essentially means, "This object is reachable and in use, do not
free it.".

When using the `jsg::V8Ref<T>` type to hold a reference to a V8 JavaScript object, or when using the
`jsg::Ref<T>` type to hold a reference to a JSG Resource Type, it is important to mark the objects
that are reachable so that the V8 Garbage Collector knows how to handle them. We use a special class
`jsg::GcVisitor` to implement this functionality.

A Resource Type that contains `jsg::Ref<T>` or `jsg::V8Ref<T>` types (along with a handful of other
visitable types) should implement a `void visitForGc(jsg::GcVisitor& visitor)` method. For example,

```cpp
class Bar: public jsg::Object {
public:
  static jsg::Ref<Bar> constructor();

  JSG_RESOURCE_TYPE(Bar) {}
};

class Foo: public jsg::Object {
public:
  static jsg::Ref<Foo> constructor();

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

The JSG mechanisms will automatically detect the `visitForGc()` implementation on a type and make use
of it.

If your Resource Type owns no ref types then implementing `visitForGc()` is not necessary. It's also
not necessary to implement if you are not concerned about the possibility of reference cycles (e.g.
object A referencing object B which references object A...).

Because ref types are strong references, any ref type that is not explicitly visited will not be
eligible for garbage collection, so failure to implement proper visitation may lead to memory leaks.
As a best practice, it is recommended that you implement `visitForGc()` on all types that contain
refs, regardless of whether or not you are concerned about reference cycles.

For `jsg::Ref<T>`, garbage collection only applies to the JavaScript wrapper around the C++ object.
From the perspective of C++, `jsg::Ref<T>` is a strong reference to a refcounted object. If you have
an unreachable cycle of `jsg::Ref<T>` references, the JavaScript objects will be garbage collected,
but the C++ objects will not be freed until the reference cycle is broken. It is critical to think
about ownership and only hold `jsg::Ref<T>` from owner to owned objects; "backwards" references
(from owned object to owner) should be regular C++ references or pointers. If it is possible for
the reference from owned object to owner object to nulled out, use a `kj::Maybe<T&>` where `T` is
the owning object type.

Aside from the `jsg::Ref<T>` and `jsg::V8Ref<T>` types, there are a handful of other types that also
support GC visitation. These types evolve over time so the list below may not be exhaustive:

* `jsg::Ref<T>`
* `jsg::V8Ref<T>` (and `jsg::Value`)
* `jsg::HashableV8Ref<T>`
* `jsg::Optional<T>` (when `T` is a GC-visitable type)
* `jsg::LenientOptional<T>` (when `T` is a GC-visitable type)
* `jsg::Name`
* `jsg::Function<Signature>`
* `jsg::Promise<T>`
* `jsg::Promise<T>::Resolver`
* `jsg::BufferSource`
* `jsg::Sequence`
* `jsg::Generator<T>`
* `jsg::AsyncGenerator<T>`
* `kj::Maybe<T>` (when `T` is a GC-visitable type)

---

# Part 4: Utilities and Helpers

This section covers utility functions, helper types, and performance optimizations
provided by JSG for common development tasks.

## Utilities

This section covers utility functions and helper types provided by JSG for common tasks
like error handling, string manipulation, and interacting with V8.

### JsValue Types

The `JsValue` family of types provides a modern abstraction layer over V8's `v8::Local<T>` handles.
These types are designed to reduce direct use of the V8 API and make code more readable and
type-safe.

#### Overview

```cpp
void example(jsg::Lock& js) {
  // Create values using jsg::Lock methods
  JsString str = js.str("hello");
  JsNumber num = js.num(42);
  JsBoolean flag = js.boolean(true);
  JsObject obj = js.obj();

  // Set properties on objects
  obj.set(js, "message", str);
  obj.set(js, "count", num);

  // Get properties
  JsValue val = obj.get(js, "message");

  // Type checking and casting
  if (val.isString()) {
    KJ_IF_SOME(s, val.tryCast<JsString>()) {
      kj::String cppStr = s.toString(js);
    }
  }
}
```

#### Key Characteristics

1. **Stack-only allocation**: `JsValue` types can only be allocated on the stack (enforced in
   debug builds). They are lightweight wrappers around V8 handles.

2. **Implicit conversion to V8 types**: All `JsValue` types can be implicitly converted to their
   underlying `v8::Local<T>` type, allowing seamless interop with V8 APIs.

3. **Implicit conversion to `JsValue`**: All specific types (`JsString`, `JsNumber`, etc.) can
   be implicitly converted to `JsValue`.

4. **Use `JsRef<T>` for persistence**: To store a JavaScript value beyond the current scope,
   use `JsRef<T>` (see below).

#### Creating Values with `jsg::Lock`

The `jsg::Lock` class provides factory methods for creating `JsValue` types:

```cpp
void createValues(jsg::Lock& js) {
  // Primitives
  JsValue undef = js.undefined();
  JsValue null = js.null();
  JsBoolean b = js.boolean(true);

  // Numbers (various overloads for different integer types)
  JsNumber n = js.num(3.14);
  JsInt32 i32 = js.num(42);           // int8_t, int16_t, int32_t
  JsUint32 u32 = js.num(42u);         // uint8_t, uint16_t, uint32_t
  JsBigInt big = js.bigInt(INT64_MAX);

  // Strings
  JsString empty = js.str();
  JsString s = js.str("hello");
  JsString interned = js.strIntern("propertyName");  // Deduplicated in V8

  // Objects and collections
  JsObject obj = js.obj();
  JsObject noProto = js.objNoProto();  // Object with null prototype
  JsMap map = js.map();
  JsArray arr = js.arr(js.num(1), js.num(2), js.num(3));
  JsSet set = js.set(js.str("a"), js.str("b"));

  // Symbols
  JsSymbol sym = js.symbol("mySymbol");
  JsSymbol shared = js.symbolShared("Symbol.for('shared')");

  // Dates
  JsDate date = js.date(1234567890.0);  // From timestamp
  JsDate kjDate = js.date(kj::UNIX_EPOCH + 1000 * kj::MILLISECONDS);

  // Errors
  JsValue err = js.error("Something went wrong");
  JsValue typeErr = js.typeError("Expected a string");
  JsValue rangeErr = js.rangeError("Value out of range");

  // Global object
  JsObject global = js.global();
}
```

#### Available Types

| Type | V8 Equivalent | Description |
|------|---------------|-------------|
| `JsValue` | `v8::Value` | Generic value, can hold any JS value |
| `JsBoolean` | `v8::Boolean` | Boolean value |
| `JsNumber` | `v8::Number` | Double-precision number |
| `JsInt32` | `v8::Int32` | 32-bit signed integer |
| `JsUint32` | `v8::Uint32` | 32-bit unsigned integer |
| `JsBigInt` | `v8::BigInt` | Arbitrary precision integer |
| `JsString` | `v8::String` | String value |
| `JsSymbol` | `v8::Symbol` | Symbol value |
| `JsObject` | `v8::Object` | Object |
| `JsArray` | `v8::Array` | Array |
| `JsMap` | `v8::Map` | Map collection |
| `JsSet` | `v8::Set` | Set collection |
| `JsFunction` | `v8::Function` | Function |
| `JsPromise` | `v8::Promise` | Promise (for state inspection only) |
| `JsDate` | `v8::Date` | Date object |
| `JsRegExp` | `v8::RegExp` | Regular expression |
| `JsArrayBuffer` | `v8::ArrayBuffer` | ArrayBuffer |
| `JsArrayBufferView` | `v8::ArrayBufferView` | TypedArray or DataView |
| `JsUint8Array` | `v8::Uint8Array` | Uint8Array |
| `JsProxy` | `v8::Proxy` | Proxy object |

#### Type Checking and Casting

`JsValue` provides numerous `is*()` methods for type checking:

```cpp
void checkTypes(jsg::Lock& js, JsValue value) {
  // Check specific types
  if (value.isString()) { /* ... */ }
  if (value.isNumber()) { /* ... */ }
  if (value.isObject()) { /* ... */ }
  if (value.isArray()) { /* ... */ }
  if (value.isFunction()) { /* ... */ }
  if (value.isPromise()) { /* ... */ }

  // Check for null/undefined
  if (value.isNull()) { /* ... */ }
  if (value.isUndefined()) { /* ... */ }
  if (value.isNullOrUndefined()) { /* ... */ }

  // Check typed arrays
  if (value.isTypedArray()) { /* ... */ }
  if (value.isUint8Array()) { /* ... */ }
  if (value.isArrayBuffer()) { /* ... */ }
}
```

Use `tryCast<T>()` to safely cast to a more specific type:

```cpp
void castExample(jsg::Lock& js, JsValue value) {
  KJ_IF_SOME(str, value.tryCast<JsString>()) {
    // str is a JsString
    kj::String cppStr = str.toString(js);
  }

  KJ_IF_SOME(arr, value.tryCast<JsArray>()) {
    // arr is a JsArray
    uint32_t len = arr.size();
  }
}
```

#### Working with Objects

```cpp
void objectOperations(jsg::Lock& js) {
  JsObject obj = js.obj();

  // Set properties
  obj.set(js, "name", js.str("Alice"));
  obj.set(js, js.symbol("id"), js.num(123));

  // Set read-only property
  obj.setReadOnly(js, "version", js.str("1.0"));

  // Get properties
  JsValue name = obj.get(js, "name");
  JsValue missing = obj.get(js, "nonexistent");  // Returns undefined

  // Check property existence
  if (obj.has(js, "name")) { /* ... */ }
  if (obj.has(js, "name", JsObject::HasOption::OWN)) { /* own property only */ }

  // Delete properties
  obj.delete_(js, "name");

  // Get all property names
  JsArray names = obj.getPropertyNames(js,
      KeyCollectionFilter::OWN_ONLY,
      PropertyFilter::ONLY_ENUMERABLE,
      IndexFilter::INCLUDE_INDICES);

  // Get prototype
  JsValue proto = obj.getPrototype(js);

  // Check if object is instance of a JSG resource type
  if (obj.isInstanceOf<MyResourceType>(js)) {
    auto ref = KJ_ASSERT_NONNULL(obj.tryUnwrapAs<MyResourceType>(js));
  }

  // Freeze/seal
  obj.seal(js);
  obj.recursivelyFreeze(js);

  // Clone via JSON (deep copy, loses non-JSON-serializable values)
  JsObject clone = obj.jsonClone(js);
}
```

#### Working with Strings

```cpp
void stringOperations(jsg::Lock& js) {
  JsString str = js.str("Hello, World!");

  // Get length
  int len = str.length(js);          // UTF-16 code units
  size_t utf8Len = str.utf8Length(js);  // UTF-8 bytes

  // Convert to KJ string
  kj::String cppStr = str.toString(js);

  // Check string properties
  bool flat = str.isFlat();
  bool oneByte = str.containsOnlyOneByte();

  // Concatenate strings
  JsString combined = JsString::concat(js, js.str("Hello, "), js.str("World!"));

  // Internalize (deduplicate)
  JsString interned = str.internalize(js);

  // Write to buffer
  kj::Array<char> buf = kj::heapArray<char>(100);
  auto status = str.writeInto(js, buf, JsString::WriteFlags::NULL_TERMINATION);
  // status.read = characters read from string
  // status.written = bytes written to buffer
}
```

#### Working with Arrays

```cpp
void arrayOperations(jsg::Lock& js) {
  // Create array with initial values
  JsArray arr = js.arr(js.num(1), js.num(2), js.num(3));

  // Or create from a C++ array
  kj::Array<int> values = kj::heapArray<int>({1, 2, 3, 4, 5});
  JsArray arr2 = js.arr(values.asPtr(), [](jsg::Lock& js, int v) {
    return js.num(v);
  });

  // Get size
  uint32_t len = arr.size();

  // Get element
  JsValue elem = arr.get(js, 0);

  // Add element
  arr.add(js, js.str("new item"));
}
```

#### Working with Functions

```cpp
void functionOperations(jsg::Lock& js, JsFunction func) {
  // Call with receiver
  JsValue result = func.call(js, js.global(), js.num(1), js.str("arg"));

  // Call without receiver (uses null, which becomes global in non-strict mode)
  JsValue result2 = func.callNoReceiver(js, js.num(42));

  // Get function properties
  size_t length = func.length(js);  // Number of declared parameters
  JsString name = func.name(js);

  // Can be used as an object
  JsObject funcAsObj = func;
  funcAsObj.set(js, "customProp", js.num(123));
}
```

#### JSON Operations

```cpp
void jsonOperations(jsg::Lock& js, JsValue value) {
  // Convert to JSON string
  kj::String json = value.toJson(js);

  // Parse JSON string
  JsValue parsed = JsValue::fromJson(js, R"({"key": "value"})");

  // Parse JSON from another JsValue (string)
  JsValue parsed2 = JsValue::fromJson(js, js.str(R"([1, 2, 3])"));
}
```

#### Structured Clone

```cpp
void cloneExample(jsg::Lock& js, JsValue value) {
  // Deep clone using structured clone algorithm
  JsValue cloned = value.structuredClone(js);

  // Clone with transferables (e.g., ArrayBuffers)
  kj::Array<JsValue> transfers = kj::heapArray<JsValue>({someArrayBuffer});
  JsValue cloned2 = value.structuredClone(js, kj::mv(transfers));
}
```

#### Persisting Values with `JsRef<T>`

`JsValue` types are only valid within the current scope. To store a JavaScript value persistently,
use `JsRef<T>`:

```cpp
class MyClass: public jsg::Object {
public:
  void storeValue(jsg::Lock& js, JsValue value) {
    stored = value.addRef(js);
  }

  JsValue getStored(jsg::Lock& js) {
    return stored.getHandle(js);
  }

  JSG_RESOURCE_TYPE(MyClass) {
    JSG_METHOD(storeValue);
    JSG_METHOD(getStored);
  }

private:
  JsRef<JsValue> stored;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(stored);  // Important for garbage collection!
  }
};
```

`JsRef<T>` can hold any `JsValue` type:

```cpp
JsRef<JsValue> genericRef;
JsRef<JsString> stringRef;
JsRef<JsObject> objectRef;
JsRef<JsFunction> functionRef;
```

#### Comparison and Equality

```cpp
void compareValues(jsg::Lock& js, JsValue a, JsValue b) {
  // Abstract equality (==)
  bool equal = (a == b);

  // Strict equality (===)
  bool strictEqual = a.strictEquals(b);

  // Truthiness
  bool truthy = a.isTruthy(js);

  // Type of
  kj::String type = a.typeOf(js);  // "string", "number", "object", etc.
}
```

#### Note on `JsPromise` vs `jsg::Promise<T>`

`JsPromise` and `jsg::Promise<T>` serve different purposes:

- **`JsPromise`**: A thin wrapper around `v8::Promise` for inspecting promise state. Cannot be
  awaited in C++. Use when you need to check if a promise is pending/fulfilled/rejected or
  access its result directly.

- **`jsg::Promise<T>`**: A higher-level abstraction for working with promises in C++. Provides
  `.then()`, `.catch_()` methods and integrates with the JSG type system. Use this for most
  promise handling.

### Serialization

JSG provides a serialization system built on V8's `ValueSerializer` that supports the
[structured clone algorithm](https://developer.mozilla.org/en-US/docs/Web/API/Web_Workers_API/Structured_clone_algorithm).
This is used for `structuredClone()`, passing data between workers, and storing data in
Durable Objects.

#### Basic Usage

For simple structured clone operations on JavaScript values:

```cpp
void cloneExample(jsg::Lock& js, JsValue value) {
  // Simple clone
  JsValue cloned = jsg::structuredClone(js, value);

  // Clone with transferables (e.g., ArrayBuffers)
  kj::Array<JsValue> transfers = kj::heapArray<JsValue>({someArrayBuffer});
  JsValue cloned2 = jsg::structuredClone(js, value, kj::mv(transfers));
}
```

#### Using Serializer and Deserializer Directly

For more control, use `Serializer` and `Deserializer` directly:

```cpp
void serializeExample(jsg::Lock& js, JsValue value) {
  // Serialize
  Serializer::Released serialized = ({
    Serializer ser(js);
    ser.write(js, value);
    ser.release();
  });

  // serialized.data contains the serialized bytes
  // serialized.sharedArrayBuffers contains any SharedArrayBuffers
  // serialized.transferredArrayBuffers contains any transferred ArrayBuffers

  // Deserialize
  JsValue result = ({
    Deserializer deser(js, serialized);
    deser.readValue(js);
  });
}
```

You can serialize multiple values:

```cpp
void multiValueExample(jsg::Lock& js) {
  Serializer ser(js);
  ser.write(js, js.str("first"));
  ser.write(js, js.num(42));
  ser.write(js, js.obj());
  auto released = ser.release();

  Deserializer deser(js, released);
  JsValue first = deser.readValue(js);   // "first"
  JsValue second = deser.readValue(js);  // 42
  JsValue third = deser.readValue(js);   // {}
}
```

#### Serializer Options

```cpp
Serializer::Options options {
  .version = kj::none,              // Override wire format version
  .omitHeader = false,              // Skip serialization header
  .treatClassInstancesAsPlainObjects = true,  // How to handle class instances
  .externalHandler = kj::none,      // Handler for external resources
};
Serializer ser(js, options);
```

#### Transferring ArrayBuffers

Use `transfer()` to move ArrayBuffers instead of copying:

```cpp
void transferExample(jsg::Lock& js, JsArrayBuffer buffer) {
  Serializer ser(js);
  ser.transfer(js, buffer);  // Mark for transfer before writing
  ser.write(js, buffer);
  auto released = ser.release();
  // buffer is now detached (neutered)

  // On the receiving side, pass the transferred buffers
  Deserializer deser(js, released.data,
      released.transferredArrayBuffers.asPtr(),
      released.sharedArrayBuffers.asPtr());
  JsValue result = deser.readValue(js);
}
```

#### Making Resource Types Serializable (`JSG_SERIALIZABLE`)

To make a JSG Resource Type serializable via structured clone, implement `serialize()` and
`deserialize()` methods and use the `JSG_SERIALIZABLE` macro:

```cpp
// Define a tag enum for your serializable types (Cap'n Proto enum recommended)
enum class SerializationTag {
  MY_TYPE_V1 = 1,
  MY_TYPE_V2 = 2,  // For versioning
};

class MyType: public jsg::Object {
public:
  MyType(uint32_t id, kj::String name): id(id), name(kj::mv(name)) {}

  static jsg::Ref<MyType> constructor(jsg::Lock& js, uint32_t id, kj::String name) {
    return js.alloc<MyType>(id, kj::mv(name));
  }

  JSG_RESOURCE_TYPE(MyType) {
    JSG_READONLY_PROTOTYPE_PROPERTY(id, getId);
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
  }

  // Serialize the object's state
  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    serializer.writeRawUint32(id);
    serializer.writeLengthDelimited(name);
  }

  // Deserialize and reconstruct the object
  static jsg::Ref<MyType> deserialize(
      jsg::Lock& js,
      SerializationTag tag,
      jsg::Deserializer& deserializer) {
    uint32_t id = deserializer.readRawUint32();
    kj::String name = deserializer.readLengthDelimitedString();
    return js.alloc<MyType>(id, kj::mv(name));
  }

  // Declare serializable with current tag (can list old tags for backwards compat)
  JSG_SERIALIZABLE(SerializationTag::MY_TYPE_V1);

private:
  uint32_t id;
  kj::String name;

  uint32_t getId() { return id; }
  kj::String getName() { return kj::str(name); }
};
```

**Important notes:**

- `JSG_SERIALIZABLE` must appear **after** the `JSG_RESOURCE_TYPE` block, not inside it
- The tag enum values must never change once data has been serialized
- `deserialize()` receives the tag so it can handle multiple versions

#### Versioning Serializable Types

To evolve a serializable type while maintaining backwards compatibility:

```cpp
enum class SerializationTag {
  MY_TYPE_V1 = 1,
  MY_TYPE_V2 = 2,
};

class MyType: public jsg::Object {
public:
  // V2 adds an optional description field
  MyType(uint32_t id, kj::String name, kj::Maybe<kj::String> description = kj::none)
      : id(id), name(kj::mv(name)), description(kj::mv(description)) {}

  // ... constructor and JSG_RESOURCE_TYPE ...

  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    // Always write the current (V2) format
    serializer.writeRawUint32(id);
    serializer.writeLengthDelimited(name);
    KJ_IF_SOME(desc, description) {
      serializer.writeRawUint32(1);  // has description
      serializer.writeLengthDelimited(desc);
    } else {
      serializer.writeRawUint32(0);  // no description
    }
  }

  static jsg::Ref<MyType> deserialize(
      jsg::Lock& js,
      SerializationTag tag,
      jsg::Deserializer& deserializer) {
    uint32_t id = deserializer.readRawUint32();
    kj::String name = deserializer.readLengthDelimitedString();

    kj::Maybe<kj::String> description;
    if (tag == SerializationTag::MY_TYPE_V2) {
      // V2 format includes optional description
      if (deserializer.readRawUint32() == 1) {
        description = deserializer.readLengthDelimitedString();
      }
    }
    // V1 format has no description field

    return js.alloc<MyType>(id, kj::mv(name), kj::mv(description));
  }

  // First tag is current version, others are accepted old versions
  JSG_SERIALIZABLE(SerializationTag::MY_TYPE_V2, SerializationTag::MY_TYPE_V1);

private:
  uint32_t id;
  kj::String name;
  kj::Maybe<kj::String> description;
};
```

#### Using TypeHandlers in Serialization

Both `serialize()` and `deserialize()` can request `TypeHandler` arguments for converting
between C++ and JavaScript types:

```cpp
void serialize(jsg::Lock& js, jsg::Serializer& serializer,
               const jsg::TypeHandler<kj::String>& stringHandler) {
  // Convert C++ string to JS string and serialize as a JS value
  serializer.write(js, JsValue(stringHandler.wrap(js, kj::str(text))));
}

static jsg::Ref<MyType> deserialize(
    jsg::Lock& js,
    SerializationTag tag,
    jsg::Deserializer& deserializer,
    const jsg::TypeHandler<kj::String>& stringHandler) {
  // Deserialize JS value and convert to C++ string
  JsValue jsVal = deserializer.readValue(js);
  kj::String text = KJ_ASSERT_NONNULL(stringHandler.tryUnwrap(js, jsVal));
  return js.alloc<MyType>(kj::mv(text));
}
```

#### Raw Read/Write Methods

The `Serializer` and `Deserializer` provide low-level methods for custom binary formats:

```cpp
// Serializer write methods
serializer.writeRawUint32(uint32_t value);
serializer.writeRawUint64(uint64_t value);
serializer.writeRawBytes(kj::ArrayPtr<const kj::byte> bytes);
serializer.writeLengthDelimited(kj::ArrayPtr<const kj::byte> bytes);  // size + bytes
serializer.writeLengthDelimited(kj::StringPtr text);
serializer.write(js, JsValue value);  // Full V8 serialization

// Deserializer read methods
uint32_t u32 = deserializer.readRawUint32();
uint64_t u64 = deserializer.readRawUint64();
kj::ArrayPtr<const kj::byte> bytes = deserializer.readRawBytes(size);
kj::ArrayPtr<const kj::byte> delimited = deserializer.readLengthDelimitedBytes();
kj::String str = deserializer.readLengthDelimitedString();
JsValue val = deserializer.readValue(js);  // Full V8 deserialization
uint32_t version = deserializer.getVersion();  // Wire format version
```

#### One-Way Serialization (`JSG_SERIALIZABLE_ONEWAY`)

For types that serialize to a different type (e.g., a legacy type that should deserialize
as a newer type):

```cpp
class LegacyType: public jsg::Object {
  // ...

  void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
    // Serialize in NewType's format
    serializer.writeRawUint32(value);
  }

  // No deserialize() method - uses NewType's deserializer
  JSG_SERIALIZABLE_ONEWAY(SerializationTag::NEW_TYPE);
};
```

### Errors

JSG provides a comprehensive error handling system that bridges C++ exceptions and JavaScript
errors. The key components are:

#### Error Macros

JSG provides macros that work similarly to KJ's assertion macros but produce JavaScript-friendly
error messages:

```cpp
// Throws a TypeError if the condition is false
JSG_REQUIRE(condition, TypeError, "Expected a valid value, got ", value);

// Like JSG_REQUIRE but returns the unwrapped value from a kj::Maybe
auto& val = JSG_REQUIRE_NONNULL(maybeValue, TypeError, "Value must not be null");

// Unconditionally throws an error
JSG_FAIL_REQUIRE(RangeError, "Index ", index, " is out of bounds");

// Like KJ_ASSERT but produces a JSG-style error
JSG_ASSERT(condition, Error, "Internal assertion failed");
```

The `jsErrorType` parameter can be one of:
- `TypeError`, `Error`, `RangeError` - Standard JavaScript error types
- `DOMOperationError`, `DOMDataError`, `DOMInvalidStateError`, etc. - DOMException types

Unlike `KJ_REQUIRE`, `JSG_REQUIRE` passes all message arguments through `kj::str()`, so you are
responsible for formatting the entire message string.

#### `JsExceptionThrown`

When C++ code needs to throw a JavaScript exception, it should:
1. Call `isolate->ThrowException()` to set the JavaScript error value
2. Throw `JsExceptionThrown()` as a C++ exception

This C++ exception is caught by JSG's callback glue before returning to V8. This approach is
more ergonomic than V8's convention of returning `v8::Maybe` values.

```cpp
void someMethod(jsg::Lock& js) {
  if (somethingWrong) {
    js.throwException(js.error("Something went wrong"));
    // Or using the lower-level API:
    // isolate->ThrowException(v8::Exception::Error(...));
    // throw JsExceptionThrown();
  }
}
```

#### `makeInternalError()` and `throwInternalError()`

These functions create JavaScript errors from internal C++ exceptions while obfuscating
sensitive implementation details:

```cpp
// Creates a JS error value, logging the full details to stderr
v8::Local<v8::Value> makeInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage);

// Creates and throws the error
void throwInternalError(v8::Isolate* isolate, kj::StringPtr internalMessage);
void throwInternalError(v8::Isolate* isolate, kj::Exception&& exception);
```

If the exception was created using `throwTunneledException()`, the original JavaScript
exception is reconstructed instead of being obfuscated.

#### `throwTypeError()`

Throws a JavaScript `TypeError` with contextual information about where the type mismatch occurred:

```cpp
// With context about the error location
throwTypeError(isolate, TypeErrorContext::methodArgument(typeid(MyClass), "doThing", 0),
               "string");

// With a free-form message
throwTypeError(isolate, "Expected a string but got a number"_kj);
```

#### `check()`

A utility template for unwrapping V8's `MaybeLocal` and `Maybe` types, throwing
`JsExceptionThrown` if the value is empty (indicating V8 has already scheduled an exception):

```cpp
// Instead of:
v8::Local<v8::String> str;
if (!maybeStr.ToLocal(&str)) {
  throw JsExceptionThrown();
}

// Use:
v8::Local<v8::String> str = check(maybeStr);
```

#### `jsg::DOMException`

`jsg::DOMException` is a JSG Resource Type that implements the standard Web IDL [DOMException][]
interface. It is the only non-builtin JavaScript exception type that standard web APIs are
allowed to throw per Web IDL.

```cpp
class DOMException: public jsg::Object {
public:
  DOMException(kj::String message, kj::String name);

  static Ref<DOMException> constructor(
      const v8::FunctionCallbackInfo<v8::Value>& args,
      Optional<kj::String> message,
      Optional<kj::String> name);

  kj::StringPtr getName() const;
  kj::StringPtr getMessage() const;
  int getCode() const;

  // Standard DOMException error codes as static constants
  static constexpr int INDEX_SIZE_ERR = 1;
  static constexpr int NOT_FOUND_ERR = 8;
  static constexpr int NOT_SUPPORTED_ERR = 9;
  // ... and many more

  JSG_RESOURCE_TYPE(DOMException) {
    JSG_INHERIT_INTRINSIC(v8::kErrorPrototype);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(name, getName);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(code, getCode);
    // Static constants...
  }
};
```

To throw a DOMException from C++ code, use the `JSG_REQUIRE` macro with the appropriate
DOMException error type:

```cpp
JSG_REQUIRE(isValid, DOMInvalidStateError, "The object is in an invalid state");
JSG_REQUIRE(hasAccess, DOMNotSupportedError, "This operation is not supported");
```

[DOMException]: https://webidl.spec.whatwg.org/#idl-DOMException

#### Tunneled Exceptions

JSG supports "tunneling" JavaScript exceptions through KJ's exception system. This allows
JavaScript exceptions to be thrown, caught as `kj::Exception`, passed across boundaries
(like RPC), and then reconstructed back into the original JavaScript exception.

To tunnel a JavaScript exception:

```cpp
// Given a JavaScript exception, create a KJ exception that can be tunneled
kj::Exception createTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);

// Or throw it directly
[[noreturn]] void throwTunneledException(v8::Isolate* isolate, v8::Local<v8::Value> exception);
```

When a tunneled exception is later converted back to JavaScript via `makeInternalError()` or
`exceptionToJs()`, the original JavaScript exception is reconstructed rather than being
replaced with a generic internal error message.

You can check if a `kj::Exception` contains a tunneled JavaScript exception:

```cpp
if (jsg::isTunneledException(exception.getDescription())) {
  // This exception originated from JavaScript and can be reconstructed
}
```

The tunneling mechanism encodes the JavaScript exception type and message in a special format
within the KJ exception description. The format uses prefixes like `jsg.TypeError:` or
`jsg.DOMException(NotFoundError):` to identify the exception type.

### `jsg::v8Str(...)` and `jsg::v8StrIntern(...)`

These utility functions create V8 string values from C++ strings:

```cpp
// Create a V8 string from a kj::StringPtr (interpreted as UTF-8)
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::StringPtr str);

// Create a V8 string from a kj::ArrayPtr<const char> (UTF-8)
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::ArrayPtr<const char> ptr);

// Create a V8 string from a kj::ArrayPtr<const char16_t> (UTF-16)
v8::Local<v8::String> v8Str(v8::Isolate* isolate, kj::ArrayPtr<const char16_t> ptr);

// Create an internalized (deduplicated) V8 string - useful for property names
v8::Local<v8::String> v8StrIntern(v8::Isolate* isolate, kj::StringPtr str);
```

**Note:** New code should prefer using the methods on `jsg::Lock` instead:

```cpp
void someMethod(jsg::Lock& js) {
  // Preferred: use js.str() instead of v8Str()
  JsString str = js.str("hello");

  // For internalized strings
  JsString internedStr = js.strIntern("propertyName");
}
```

#### External Strings

For static constant strings that will never be deallocated, you can use external strings
which avoid copying the data into V8's heap:

```cpp
// For Latin-1 encoded static strings (NOT UTF-8!)
v8::Local<v8::String> newExternalOneByteString(Lock& js, kj::ArrayPtr<const char> buf);

// For UTF-16 encoded static strings
v8::Local<v8::String> newExternalTwoByteString(Lock& js, kj::ArrayPtr<const uint16_t> buf);
```

**Important:** The `OneByteString` variant interprets the buffer as Latin-1, not UTF-8. For
text outside the Latin-1 range, you must use the two-byte (UTF-16) variant.

## V8 Fast API

V8 Fast API allows V8 to compile JavaScript code that calls native functions in a way that directly
jumps to the C++ implementation without going through the usual JavaScript-to-C++ binding layers.
This is accomplished by having V8 generate specialized machine code that knows how to call the C++
function directly, skipping the overhead of value conversion and JavaScript calling conventions.

### Requirements for Fast API compatibility

For a method to be compatible with Fast API, it must adhere to several constraints due to V8 itself,
and may change as the Fast API mechanism continues to evolve:

1. **Return Type**: Must be one of these primitive types: `void`, `bool`, `int32_t`, `uint32_t`,
   `float`, or `double`.

2. **Parameter Types**: Can be the primitive types listed above, or V8 handle types like
   `v8::Local<v8::Value>` or `v8::Local<v8::Object>`, or types that can be unwrapped from a V8
   value using the TypeWrapper.

3. **Method Structure**: Can be a regular instance method, a const instance method, or a method
   that takes `jsg::Lock&` as its first parameter.

### Using Fast API in workerd

By default, any `JSG_METHOD(name)` will execute the fast path if the method signature is compatible
with Fast API requirements. To explicitly assert that a function uses V8 Fast API in workerd, you
can use `JSG_ASSERT_FASTAPI`:

```cpp
JSG_ASSERT_FASTAPI(MyClass::myMethod);
```

This will produce a compile-time error if the method signature is not Fast API compatible.

### How it works

When a method is registered with the V8 Fast API, workerd automatically:

1. Checks if the method signature is compatible with Fast API requirements
2. Registers both a regular (slow path) method handler and a Fast API handler
3. Lets V8 optimize calls to this method when possible

V8 determines at runtime whether to use the fast or slow path:

- The fast path is used when the method is called from optimized code
- The slow path is used when called from unoptimized code or when handling complex cases

["KJ Style Guide"]: https://github.com/capnproto/capnproto/blob/master/style-guide.md
["KJ Tour"]: https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md
[Record]: https://webidl.spec.whatwg.org/#idl-record
[Sequence]: https://webidl.spec.whatwg.org/#idl-sequence

---

# Part 5: TypeScript and Async

This section covers TypeScript definition generation and asynchronous programming
support in JSG.

## TypeScript

TypeScript definitions are automatically generated from JSG RTTI using scripts in the `/types`
directory. To control auto-generation, JSG provides 3 macros for use inside a `JSG_RESOURCE_TYPE`
block: `JSG_TS_ROOT`, `JSG_TS_OVERRIDE`, `JSG_TS_DEFINE`. There are also struct variants of each
macro (`JSG_STRUCT_TS_ROOT`, `JSG_STRUCT_TS_OVERRIDE` and `JSG_STRUCT_TS_DEFINE`), that should
be placed adjacent to the `JSG_STRUCT` declaration, inside the same `struct` definition.

### `JSG_TS_ROOT`/`JSG_STRUCT_TS_ROOT`

Declares that this type should be considered a "root" for the purposes of automatically generating
TypeScript definitions. All "root" types and their recursively referenced types (e.g. method
parameter/return types, property types, inherits, etc) will be included in the generated
TypeScript. For example, the type `ServiceWorkerGlobalScope` should be a "root", as should any non-
global type that we'd like include (e.g. `ExportedHandler` and capabilities such as `KvNamespace`).

The reason we even have to define roots in the first place (as opposed to just generating
TypeScript definitions for all isolate types) is that some types should only be included when
certain compatibility flags are enabled. For example, we only want to include the `Navigator` and
spec-compliant URL implementation types if `global_navigator` and `url_standard` are enabled
respectively.

Note roots are visited before overrides, so if an override references a new type that wasn't
already referenced by the original type or any other rooted type, the referenced type will itself
need to be declared a root (e.g. `HTMLRewriter`'s HTML Content Tokens like `Element`).

### `JSG_TS_OVERRIDE`/`JSG_STRUCT_TS_OVERRIDE`

Customises the generated TypeScript definition for this type. This macro accepts a single override
parameter containing a partial TypeScript statement definition. Varargs are accepted so that
overrides can contain `,` outside of balanced brackets. After trimming whitespace from the override,
the following rules are applied:

> :warning:	**WARNING:** there's a _lot_ of magic here, ensure you understand the examples before
> writing overrides

1. If an override starts with `export `, `declare `, `type `, `abstract `, `class `, `interface `,
   `enum `, `const `, `var ` or `function `, the generated definition will be replaced with the
   override. If the replacement is a `class`, `enum`, `const`, `var` or `function`, the `declare`
   modifier will be added if it's not already present. In the special case that the override is a
   `type`-alias to `never`, the generated definition will be deleted.

2. Otherwise, the override will be converted to a TypeScript class as follows: (where `<n>` is the
   unqualified C++ type name of this resource type)

   1. If an override starts with `extends `, `implements ` or `{`: `class <n> ` will be prepended
   2. If an override starts with `<`: `class <n>` will be prepended
   3. Otherwise, `class ` will be prepended

   After this, if the override doesn't end with `}`, ` {}` will be appended.

   The class is then parsed and merged with the generated definition as follows: (note that even
   though we convert all non-replacement overrides to classes, this type classification is ignored
   when merging, classes just support all possible forms of override)

   1. If the override class has a different name to the generated type, the generated type is
      renamed and all references to it are updated. Note that the renaming happens after all
      overrides are applied, so you're able to reference original C++ names in other types'
      overrides.
   2. If the override class defines type parameters, they are copied to the generated type.
   3. If the override class defines heritage clauses (e.g. `extends`/`implements`), they replace the
      generated types'. Notably, these may include type arguments.
   4. If the override class defines members, those are merged with the generated type's members as
      follows:

      1. Members in the override but not in the generated type are inserted at the end
      2. If the override has a member with the same name as a member in the generated type, the
         generated member is removed, and the override is inserted instead. Note that static and
         instance members belong to different namespaces for the purposes of this comparison.
      3. If an override member property is declared type `never`, it is not inserted, but its
         presence may remove the generated member (as per 2).

Note that overrides can only customise a single definition. To add additional, handwritten,
TypeScript-only definitions, use the `JSG_(STRUCT_)TS_DEFINE` macros.

These macros can also be called conditionally in `JSG_RESOURCE_TYPE` blocks based on compatibility
flags. To define a compatibility-flag dependent `JSG_STRUCT` override, define a full-type
replacement `struct` override to a `never` type alias (i.e. `JSG_STRUCT_TS_OVERRIDE(type MyStruct = never)`)
to delete the original definition, then use `JSG_TS_DEFINE` in a nearby `JSG_RESOURCE_TYPE` block
to define an `interface` for the `struct` conditionally.

Here are some example overrides demonstrating these rules:

- ```ts
  KVNamespaceListOptions
  ```
  Renames the generated type to `KVNamespaceListOptions` and updates all type references to the new name.

- ```ts
  KVNamespace {
    get(key: string, type: "string"): Promise<string | null>;
    get(key: string, type: "arrayBuffer"): Promise<ArrayBuffer | null>;
  }
  ```
  Renames the generated type to `KVNamespace` (updating all references), and replaces the `get()` method
  definition with two overloads. Leaves all other members untouched.

- ```ts
  { json<T>(): Promise<T> }
  ```
  Replaces the `json()` method definition so it's generic in type parameter `T`. Leaves all other
  members untouched.

- ```ts
  class Body { json<T>(): Promise<T> }
  ```
  Because it starts with `class `, this override replaces the entire generated definition with
  `declare class Body { json<T>(): Promise<T> }`, removing all other members.

- ```ts
  <R = any> {
    read(): Promise<ReadableStreamReadResult<R>>;
    tee(): [ReadableStream<R>, ReadableStream<R>];
  }
  ```
  Adds a type parameter `R` which defaults to `any` to the generated type, replacing the `read()` and
  `tee()` method definitions, leaving all other members untouched.

- ```ts
  { actorState: never }
  ```
  Removes the `actorState` member from the generated type, leaving all other members untouched.

- ```ts
  extends EventTarget<WorkerGlobalScopeEventMap>
  ```
  Adds `WorkerGlobalScopeEventMap` as a type argument to the generated type's heritage. Leaves all
  members untouched.

- ```ts
  extends TransformStream<ArrayBuffer | ArrayBufferView, Uint8Array> {
    constructor(format: "gzip" | "deflate" | "deflate-raw");
  }
  ```
  Adds `ArrayBuffer | ArrayBufferView` and `Uint8Array` as type arguments to the generated type's
  heritage, then replaces the generated constructor definition, leaving all other members untouched.

- ```ts
  const WebSocketPair: {
    new (): { 0: WebSocket; 1: WebSocket };
  }
  ```
  Replaces the generated `WebSocketPair` definition with `declare const WebSocketPair: { ... };`.

- ```ts
  type ReadableStreamReadResult<R = any> =
    | { done: false; value: R; }
    | { done: true; value?: undefined; }
  ```
  Replaces the generated `ReadableStreamReadResult` definition with a type alias.

- ```ts
  type TransactionOptions = never
  ```
  Removes the generated `TransactionOptions` definition from the output. Any references to
  the type will be renamed to `TransactionOptions`. This is useful if you need declare a
  compatibility-flag dependent override for a `JSG_STRUCT` in a nearby `JSG_RESOURCE_TYPE`.

### `JSG_TS_DEFINE`/`JSG_STRUCT_TS_DEFINE`

Inserts additional TypeScript definitions next to the generated TypeScript definition for this
type. This macro accepts a single define parameter containing one or more TypeScript definitions
(e.g. `interface`s, `class`es, `type` aliases, `const`s, ...). Varargs are accepted so that defines
can contain `,` outside of balanced brackets. This macro can only be used once per
`JSG_RESOURCE_TYPE` block/`JSG_STRUCT` definition. The `declare` modifier will be added to any
`class`, `enum`, `const`, `var` or `function` definitions if it's not already present.

## Async Context Tracking

JSG provides a mechanism for rudimentary async context tracking to support the implementation
of async local storage. This is provided by the `workerd::jsg::AsyncContextFrame` class defined in
`src/workerd/jsg/async-context.h`.

`AsyncContextFrame`s form a logical stack. For every `v8::Isolate` there is always a root frame.
"Entering" a frame means pushing it to the top of the stack, making it "current". Exiting
a frame means popping it back off the top of the stack.

Every `AsyncContextFrame` has a storage context. The storage context is a map of multiple
individual storage cells, each tied to an opaque key.

When a new `AsyncContextFrame` is created, the storage context of the current is propagated to the
new resource's storage context.

All JavaScript promises, timers, and microtasks propagate the async context. In JavaScript,
we have implemented the `AsyncLocalStorage` and `AsyncResource` APIs for this purpose.
For example:

```js
import { default as async_hooks } from 'node:async_hooks';
const { AsyncLocalStorage } = async_hooks;

const als = new AsyncLocalStorage();

als.run(123, () => scheduler.wait(10)).then(() => {
  console.log(als.getStore());  // 123
});
console.log(als.getStore());  // undefined
```

Within C++, any type can act as an async resource by acquiring a reference to the current `jsg::AsyncContextFrame`.

```cpp
jsg::Lock& js = ...;

auto maybeAsyncContext = jsg::AsyncContextFrame::current(js);
                   // or jsg::AsyncContextFrame::currentRef(js) to get a jsg::Ref

// enter the async resource scope whenever necessary...
{
  jsg::AsyncContextFrame::Scope asyncScope(js, maybeAsyncContext);
  // run some code synchronously that requires the async context
}
// The async scope will exit automatically...
```

Within C++, The `jsg::AsyncContextFrame::StorageScope` can be used to temporarily set the stored
value associated with a given storage key in a new storage context independently of the Node.js API:

```cpp
jsg::Lock& js = ...
jsg::Value value = ...

// The storage key must be kj::Refcounted.
kj::Own<AsyncContextFrame::StorageKey> key =
    kj::refcounted<AsyncContextFrame::StorageKey>();
// When you're done with the storage key and know it will not be reused,
// reset it to clear the storage cell.
KJ_DEFER(key->reset());

{
  jsg::AsyncContextFrame::StorageScope(js, *key, value);
  // run some code synchronously. The relevant parts of the code
  // will capture the current async context as necessary.
}

// The storage cell will be automatically reset to the previous context
// with the scope exits.
```

---

# Part 6: Advanced Topics

This section covers advanced JSG features including memory tracking, internal
implementation details, performance optimization, and the module system.

## `jsg::MemoryTracker` and heap snapshot detail collection

The `jsg::MemoryTracker` is used to integrate with v8's `BuildEmbedderGraph` API.
It constructs the graph of embedder objects to be included in a generated
heap snapshot.

The API is implemented using a visitor pattern. V8 calls the `BuilderEmbedderGraph`
callback (set in `setup.h`) which in turn begins walking through the known embedder
objects collecting the necessary information include in the heap snapshot.
Generally this information consists only of the names of fields, and the sizes of
any memory-retaining values represented by the fields (e.g. heap allocations).

To instrument a struct or class so that it can be included in the graph, the
type must implement *at least* the following three methods:

 * `kj::StringPtr jsgGetMemoryName() const;`
 * `size_t jsgGetMemorySelfSize() const;`
 * `void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;`

The `jsgGetMemoryName()` method returns the name that will be used to identify
the type in the graph. This will be prefixed with `workerd / ` in the actual
generated snapshot. For instance, if this method returns `"Foo"_kjc`, the heap
snapshot will contain `"workerd / Foo"`.

The `jsgGetMemorySelfSize()` method returns the *shallow* size of the type.
This would typically be implemented using `sizeof(Type)`, and in the vast
majority of cases that's all it does. It is provided as a method, however,
in order to allow a type the ability to customize the size calculation.

The `jsgGetMemoryInfo(...)` method is the method that is actually called to
collect details for the graph. Note that this method is NOT expected to be called
within the scope of an `IoContext`. It *will* be called while within the isolate
lock, however.

Types may also implement the following additional methods to further customize
how they are represented in the graph:

 * `v8::Local<v8::Object> jsgGetMemoryInfoWrapperObject();`
 * `MemoryInfoDetachedState jsgGetMemoryInfoDetachedState() const;`
 * `bool jsgGetMemoryInfoIsRootNode() const;`

Note that the `jsgGetMemoryInfoWrapperObject()` method is called from within
a `v8::HandleScope`.

For extremely simple cases, the `JSG_MEMORY_INFO` macro can be used to simplify
implementing these methods. It is a shortcut that provides basic implementations
of the `jsgGetMemoryName()` and `jsgGetMemorySelfSize()` methods:

```cpp
JSG_MEMORY_INFO(Foo) {
  tracker.trackField("bar", bar);
}
```

... is equivalent to:

```cpp
  kj::StringPtr jsgGetMemoryName() const { return "Foo"_kjc; }

  size_t jsgGetMemorySelfSize() const { return sizeof(Foo); }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("bar", bar);
  }
```

All `jsg::Object` instances provide a basic implementation of these methods.
Within a `jsg::Object`, your only responsibility would be to implement the
helper `visitForMemoryInfo(jsg::MemoryTracker& tracker) const` method only
if the type has additional fields that need to be tracked. This works a
lot like the `visitForGc(...)` method used for GC tracing:

```cpp
class Foo : public jsg::Object {
public:
  JSG_RESOURCE_TYPE(Foo) {}
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("bar", bar);
  }
  // ...
};
```

The constructed graph should include any fields that materially contribute
the retained memory of the type. This graph is primarily used for analysis
and investigation of memory issues in an application (e.g. hunting down
memory leaks, detecting bugs, optimizing memory usage, etc) so the information
should include details that are most useful for those purposes.
This code is only ever called when a heap snapshot is being generated so
typically it should have very little cost. Heap snapshots are generally
fairly expensive to create, however, so care should be taken not to make
things too complicated. Ideally, none of the implementation methods in a
type should allocate. There is some allocation occurring internally while
building the graph, of course, but the methods for visitation (in particular
the `jsgGetMemoryInfo(...)` method) should not perform any allocations if it
can be avoided.

## Script Utilities

JSG provides script compilation utilities in `script.h` for compiling and running non-module
JavaScript code.

### `jsg::NonModuleScript`

`NonModuleScript` wraps a `v8::UnboundScript` - a script that has been compiled but is not yet
bound to a specific context:

```cpp
#include <workerd/jsg/script.h>

void runScript(jsg::Lock& js) {
  // Compile a script
  auto script = jsg::NonModuleScript::compile(js,
      "console.log('Hello, World!'); return 42;",
      "my-script.js");

  // Run the script (binds to current context and executes)
  script.run(js);

  // Or run and get the return value
  jsg::JsValue result = script.runAndReturn(js);
}
```

Key features:
- Scripts can be compiled once and run multiple times
- Each run binds the unbound script to the current context
- The script name is used for stack traces and debugging

## URL Utilities

JSG provides WHATWG-compliant URL parsing in `url.h`, powered by the ada-url library.

### `jsg::Url`

```cpp
#include <workerd/jsg/url.h>

void parseUrls() {
  // Parse a URL
  KJ_IF_SOME(url, jsg::Url::tryParse("https://example.com:8080/path?query=1#hash")) {
    // Access components
    kj::ArrayPtr<const char> protocol = url.getProtocol();  // "https:"
    kj::ArrayPtr<const char> hostname = url.getHostname();  // "example.com"
    kj::ArrayPtr<const char> port = url.getPort();          // "8080"
    kj::ArrayPtr<const char> pathname = url.getPathname();  // "/path"
    kj::ArrayPtr<const char> search = url.getSearch();      // "?query=1"
    kj::ArrayPtr<const char> hash = url.getHash();          // "#hash"
    kj::Array<const char> origin = url.getOrigin();         // "https://example.com:8080"

    // Modify components
    url.setPathname("/new/path");
    url.setSearch(kj::Maybe<kj::ArrayPtr<const char>>("?new=query"));

    // Get the full href
    kj::ArrayPtr<const char> href = url.getHref();
  }

  // Parse with a base URL
  KJ_IF_SOME(resolved, jsg::Url::tryParse("../other", "https://example.com/path/")) {
    // resolved is "https://example.com/other"
  }

  // Check if a string can be parsed without creating a Url object
  if (jsg::Url::canParse("https://example.com")) {
    // Valid URL
  }

  // Using the literal operator
  jsg::Url url = "https://example.com"_url;
}
```

#### URL Comparison

```cpp
void compareUrls(const jsg::Url& a, const jsg::Url& b) {
  // Basic equality
  if (a == b) { /* same URLs */ }

  // Comparison with options
  using Option = jsg::Url::EquivalenceOption;

  // Ignore fragments when comparing
  if (a.equal(b, Option::IGNORE_FRAGMENTS)) { /* same ignoring #hash */ }

  // Ignore search params
  if (a.equal(b, Option::IGNORE_SEARCH)) { /* same ignoring ?query */ }

  // Normalize percent-encoding in paths
  if (a.equal(b, Option::NORMALIZE_PATH)) { /* %66oo == foo */ }

  // Combine options
  if (a.equal(b, Option::IGNORE_FRAGMENTS | Option::IGNORE_SEARCH)) { /* ... */ }
}
```

### `jsg::UrlSearchParams`

Parse and manipulate URL query parameters:

```cpp
void searchParams() {
  KJ_IF_SOME(params, jsg::UrlSearchParams::tryParse("foo=1&bar=2&foo=3")) {
    // Get values
    KJ_IF_SOME(value, params.get("foo")) {
      // value is "1" (first occurrence)
    }

    // Get all values for a key
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

    // Sort and stringify
    params.sort();
    kj::Array<const char> str = params.toStr();  // "baz=4&foo=new"
  }
}
```

### `jsg::UrlPattern`

Compile and use URL patterns for routing:

```cpp
void urlPatterns() {
  // Compile a pattern from a string
  auto result = jsg::UrlPattern::tryCompile("/users/:id");
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(pattern, jsg::UrlPattern) {
      // Access compiled components
      auto& pathname = pattern.getPathname();
      kj::StringPtr patternStr = pathname.getPattern();  // "/users/:id"
      kj::StringPtr regex = pathname.getRegex();          // Generated regex
      auto names = pathname.getNames();                   // ["id"]
    }
    KJ_CASE_ONEOF(error, kj::String) {
      // Pattern compilation failed
      KJ_LOG(ERROR, "Invalid pattern", error);
    }
  }

  // Compile with options
  jsg::UrlPattern::CompileOptions options {
    .baseUrl = "https://example.com",
    .ignoreCase = true,
  };
  auto result2 = jsg::UrlPattern::tryCompile("/path", options);
}
```

## RTTI (Runtime Type Information)

JSG provides a runtime type information system in `rtti.h` that produces Cap'n Proto descriptions
of JSG structs, resources, and their members. This is used to generate TypeScript definitions,
dynamically invoke methods, perform fuzzing, and check backward compatibility.

### Overview

The RTTI system introspects JSG types at runtime and produces structured metadata using the
schema defined in `rtti.capnp`. This metadata includes:

- Type information (primitives, arrays, optionals, unions, etc.)
- Structure definitions (for `JSG_STRUCT` and `JSG_RESOURCE` types)
- Method signatures
- Property definitions

### Using the RTTI Builder

```cpp
#include <workerd/jsg/rtti.h>

// Your configuration type (often CompatibilityFlags::Reader)
using Config = CompatibilityFlags::Reader;

void inspectTypes(const Config& config) {
  jsg::rtti::Builder<Config> rtti(config);

  // Get type information for a primitive
  auto intType = rtti.type<int>();

  // Get structure information for a JSG type
  auto myClassInfo = rtti.structure<MyClass>();

  // Access structure members
  for (auto member : myClassInfo.getMembers()) {
    KJ_LOG(INFO, "Member:", member.getName());
  }

  // Lookup structure by name
  KJ_IF_SOME(structInfo, rtti.structure("MyClass"_kj)) {
    // Use structInfo...
  }
}
```

### Generated Metadata

The RTTI system produces metadata in Cap'n Proto format (`rtti.capnp`):

```
# Type discriminator
struct Type {
  union {
    unknown @0 :Void;
    voidt @1 :Void;
    boolt @2 :Void;
    number @3 :NumberType;
    string @4 :StringType;
    object @5 :Void;
    array @6 :Type;
    maybe @7 :Type;
    dict @8 :DictType;
    oneOf @9 :List(Type);
    promise @10 :Type;
    structure @11 :Text;  # Reference by name
    intrinsic @12 :Intrinsic;
    function @13 :FunctionType;
    jsgImpl @14 :JsgImplType;
  }
}

# Structure definition
struct Structure {
  name @0 :Text;
  members @1 :List(Member);
  extends @2 :Text;
  iterable @3 :Bool;
  asyncIterable @4 :Bool;
  # ... more fields
}
```

### TypeScript Generation

The primary use of RTTI is generating TypeScript definitions. The scripts in `/types` directory
use RTTI to produce `.d.ts` files for the Workers runtime API.

The generation process:
1. Instantiate RTTI builder with appropriate configuration
2. Iterate through all registered types
3. Convert Cap'n Proto metadata to TypeScript syntax
4. Apply any `JSG_TS_OVERRIDE` customizations

### Supported Type Mappings

| C++ Type | RTTI Kind |
|----------|-----------|
| `void` | `voidt` |
| `bool` | `boolt` |
| `int`, `double`, etc. | `number` |
| `kj::String`, `USVString` | `string` |
| `kj::Array<T>` | `array` |
| `kj::Maybe<T>` | `maybe` |
| `kj::OneOf<T...>` | `oneOf` |
| `jsg::Promise<T>` | `promise` |
| `jsg::Dict<V, K>` | `dict` |
| `JSG_STRUCT` types | `structure` |
| `JSG_RESOURCE` types | `structure` |
| `jsg::Function<T>` | `function` |

## Web IDL Type Mapping

JSG provides type traits and concepts in `web-idl.h` to help validate and map between C++ types
and Web IDL types. This is primarily used internally for validating `kj::OneOf` union types
against Web IDL's union constraints.

### Type Categories

Web IDL defines nine distinguishable type categories. JSG maps these to C++ concepts:

| Web IDL Category | JSG Concept | C++ Types |
|------------------|-------------|-----------|
| Boolean | `BooleanType` | `bool`, `NonCoercible<bool>` |
| Numeric | `NumericType` | `int`, `double`, `uint32_t`, etc. |
| String | `StringType` | `kj::String`, `USVString`, `DOMString`, `JsString` |
| Object | `ObjectType` | `v8::Local<v8::Object>`, `v8::Global<v8::Object>` |
| Symbol | `SymbolType` | (not yet implemented) |
| Interface-like | `InterfaceLikeType` | `JSG_RESOURCE` types, `BufferSource` |
| Callback function | `CallbackFunctionType` | `kj::Function<T>`, `Constructor<T>` |
| Dictionary-like | `DictionaryLikeType` | `JSG_STRUCT` types, `Dict<V, K>` |
| Sequence-like | `SequenceLikeType` | `kj::Array<T>`, `Sequence<T>` |

### Union Type Validation

When you use `kj::OneOf<T...>` in JSG API signatures, the `UnionTypeValidator` template
validates at compile-time that your union is Web IDL-compliant:

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

### Key Validation Rules

Per Web IDL specification:

1. At most one boolean type
2. At most one numeric type
3. At most one string type
4. At most one object type (and no interface-like, callback, dictionary-like, or sequence-like)
5. At most one callback function type
6. At most one dictionary-like type
7. At most one sequence-like type
8. At most one nullable (`kj::Maybe`) or dictionary type combined
9. No duplicate types
10. No `Optional<T>` types (use `kj::Maybe<T>` for nullable)

### Using the Concepts

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

### Nullable Type Counting

The `nullableTypeCount<T...>` template recursively counts nullable types in a union,
including through nested `kj::OneOf` types:

```cpp
// Count = 1
nullableTypeCount<kj::Maybe<int>>

// Count = 2 (nested nullables)
nullableTypeCount<kj::Maybe<kj::OneOf<kj::Maybe<int>, kj::String>>>
```

## CompileCache

The `CompileCache` provides a process-lifetime in-memory cache for V8 compilation data,
specifically designed for built-in JavaScript modules. Caching compilation data avoids
re-parsing and re-compiling the same code repeatedly.

**Important:** This cache is only appropriate for built-in modules. Entries are never removed
or replaced during the process lifetime.

### Usage

```cpp
#include <workerd/jsg/compile-cache.h>

void compilingModule(v8::Isolate* isolate, kj::StringPtr moduleSource) {
  // Get the singleton cache instance
  const jsg::CompileCache& cache = jsg::CompileCache::get();

  // Use a unique key for this module (typically the module specifier)
  kj::StringPtr cacheKey = "node:fs";

  // Check if cached compilation data exists
  KJ_IF_SOME(cachedData, cache.find(cacheKey)) {
    // Use cached data for compilation
    auto v8CachedData = cachedData.AsCachedData();
    v8::ScriptCompiler::Source source(sourceStr, v8::ScriptOrigin(...),
                                       v8CachedData.release());
    // Compile with cached data...
  } else {
    // Compile without cache
    v8::ScriptCompiler::Source source(sourceStr, v8::ScriptOrigin(...));
    auto module = v8::ScriptCompiler::CompileModule(isolate, &source,
        v8::ScriptCompiler::kEagerCompile);

    // Store the generated cache data for future use
    if (source.GetCachedData() != nullptr) {
      cache.add(cacheKey, std::shared_ptr<v8::ScriptCompiler::CachedData>(
          source.GetCachedData()));
    }
  }
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

### Thread Safety

The cache is internally mutex-guarded and safe for concurrent access from multiple threads.

## Wrappable Internals

This section covers the internal implementation details of how JSG connects C++ objects to
JavaScript. Understanding these internals is helpful for debugging, performance optimization,
and advanced use cases.

### The `Wrappable` Base Class

`Wrappable` is the base class for all C++ objects that can be exposed to JavaScript. It manages
the connection between a C++ object and its JavaScript "wrapper" object.

#### Key Concepts

1. **Lazy Wrapper Creation**: JavaScript wrappers are created on-demand when a C++ object is
   first passed to JavaScript, not when the C++ object is constructed.

2. **Dual Reference Counting**:
   - The `Wrappable` is ref-counted via `kj::Refcounted`. When a JS wrapper exists, it holds
     a reference, keeping the C++ object alive.
   - A second "strong ref" count tracks `jsg::Ref<T>` pointers that aren't visible to GC tracing.
     While this count is non-zero, the JS wrapper won't be garbage-collected.

3. **Identity Preservation**: The same C++ object always returns the same JS wrapper, preserving
   object identity and any monkey-patches the script may have applied.

#### Internal Fields

JavaScript wrapper objects have two internal fields:

```cpp
enum InternalFields : int {
  WRAPPABLE_TAG_FIELD_INDEX = 0,    // Contains WORKERD_WRAPPABLE_TAG to identify our objects
  WRAPPED_OBJECT_FIELD_INDEX = 1,   // Pointer back to the Wrappable
  INTERNAL_FIELD_COUNT = 2,
};
```

You can check if an object is a workerd API object:

```cpp
if (jsg::Wrappable::isWorkerdApiObject(object)) {
  // This is one of our wrapped C++ objects
}
```

### Context Embedder Data Slots

JSG uses V8's context embedder data to store pointers to important objects:

```cpp
enum class ContextPointerSlot : int {
  RESERVED = 0,              // Never use slot 0 (V8 reserves it)
  GLOBAL_WRAPPER = 1,        // Pointer to the global object wrapper
  MODULE_REGISTRY = 2,       // Pointer to the module registry
  EXTENDED_CONTEXT_WRAPPER = 3,  // Extended type wrapper for this context
  VIRTUAL_FILE_SYSTEM = 4,   // Virtual file system
  RUST_REALM = 5,            // Rust realm pointer
};
```

Access these slots with the helper functions:

```cpp
// Set a pointer
jsg::setAlignedPointerInEmbedderData(context, ContextPointerSlot::MODULE_REGISTRY, registry);

// Get a pointer
KJ_IF_SOME(registry, jsg::getAlignedPointerFromEmbedderData<ModuleRegistry>(
    context, ContextPointerSlot::MODULE_REGISTRY)) {
  // Use registry...
}
```

### `HeapTracer`

`HeapTracer` implements V8's `EmbedderRootsHandler` interface to integrate JSG's C++ object
graph with V8's garbage collector.

Key responsibilities:
- Tracks all `Wrappable` objects that have JavaScript wrappers
- Decides which wrappers can be collected during GC
- Manages a freelist of reusable wrapper shim objects

```cpp
// Check if we're currently in a GC destructor
if (jsg::HeapTracer::isInCppgcDestructor()) {
  // Be careful - we're being destroyed during GC
}
```

### Wrapper Lifecycle

```
1. C++ object created (no JS wrapper yet)
         ↓
2. Object passed to JavaScript
         ↓
3. attachWrapper() creates JS wrapper
         ↓
4. JS wrapper and C++ object linked
         ↓
5. GC may collect wrapper if:
   - No JS references exist
   - No strong Ref<T>s exist
   - Wrapper is "unmodified"
         ↓
6. If wrapper collected but C++ object still alive:
   - New wrapper created on next JS access
         ↓
7. When C++ object destroyed:
   - detachWrapper() called
   - JS wrapper becomes empty shell
```

### Strong vs Weak References

```cpp
class MyClass: public jsg::Object {
public:
  // Strong reference - prevents GC of the wrapper
  jsg::Ref<OtherClass> strongRef;

  // Weak reference - allows GC, must check before use
  kj::Maybe<OtherClass&> weakRef;

  void visitForGc(jsg::GcVisitor& visitor) {
    // Only visit strong refs that should prevent GC
    visitor.visit(strongRef);
    // Don't visit weakRef - it's allowed to be collected
  }
};
```

### Async Destructor Safety

JSG enforces that JavaScript heap objects don't hold KJ I/O objects directly, as this could
cause issues during garbage collection:

```cpp
// This macro helps catch bugs where I/O objects are incorrectly stored
DISALLOW_KJ_IO_DESTRUCTORS_SCOPE;

// If you need to store I/O objects, use IoOwn<T> which handles this correctly
```

## Observers

JSG provides an observer system for monitoring various runtime events such as module resolution,
compilation, and exceptions. These observers are useful for metrics collection, debugging, and
implementing custom behavior.

### `IsolateObserver`

`IsolateObserver` is the main observer interface that combines `CompilationObserver`,
`ResolveObserver`, and `InternalExceptionObserver`. You pass an `IsolateObserver` when creating
an isolate:

```cpp
class MyObserver: public jsg::IsolateObserver {
public:
  // Override methods from CompilationObserver, ResolveObserver, etc.
};

auto observer = kj::heap<MyObserver>();
MyIsolate isolate(v8System, kj::mv(observer));
```

#### Monitoring Dynamic Code Generation

```cpp
class MyObserver: public jsg::IsolateObserver {
public:
  void onDynamicEval(v8::Local<v8::Context> context,
                     v8::Local<v8::Value> source,
                     jsg::IsCodeLike isCodeLike) override {
    // Called when eval(), new Function(), or similar is used
    // isCodeLike indicates if source implements the "code-like" interface
    KJ_LOG(WARNING, "Dynamic code generation detected");
  }
};
```

### `CompilationObserver`

Monitors script and module compilation events. The `onXxxStart` methods return an
`kj::Own<void>` that is destroyed when compilation completes, allowing RAII-style timing.

```cpp
class MyObserver: public jsg::IsolateObserver {
public:
  // ESM module compilation
  kj::Own<void> onEsmCompilationStart(v8::Isolate* isolate,
                                       kj::StringPtr name,
                                       Option option) const override {
    auto startTime = kj::systemCoarseMonotonicClock().now();
    return kj::defer([startTime, name = kj::str(name)]() {
      auto duration = kj::systemCoarseMonotonicClock().now() - startTime;
      KJ_LOG(INFO, "ESM compilation", name, duration / kj::MILLISECONDS, "ms");
    });
  }

  // Non-ESM script compilation
  kj::Own<void> onScriptCompilationStart(v8::Isolate* isolate,
                                          kj::Maybe<kj::StringPtr> name) const override {
    return kj::Own<void>();  // No-op
  }

  // WebAssembly compilation
  kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate,
                                        size_t codeSize) const override {
    KJ_LOG(INFO, "Compiling WASM", codeSize, "bytes");
    return kj::Own<void>();
  }

  // WebAssembly compilation from cache
  kj::Own<void> onWasmCompilationFromCacheStart(v8::Isolate* isolate) const override {
    return kj::Own<void>();
  }

  // JSON module parsing
  kj::Own<void> onJsonCompilationStart(v8::Isolate* isolate,
                                        size_t inputSize) const override {
    return kj::Own<void>();
  }

  // Compile cache events
  void onCompileCacheFound(v8::Isolate* isolate) const override {
    // Called when cached compilation data is found
  }

  void onCompileCacheRejected(v8::Isolate* isolate) const override {
    // Called when cached compilation data is rejected (e.g., version mismatch)
  }

  void onCompileCacheGenerated(v8::Isolate* isolate) const override {
    // Called when new compilation cache data is generated
  }

  void onCompileCacheGenerationFailed(v8::Isolate* isolate) const override {
    // Called when compilation cache generation fails
  }
};
```

The `Option` enum indicates the compilation context:
- `Option::BUNDLE` - Compiling user code from the worker bundle
- `Option::BUILTIN` - Compiling built-in runtime modules

### `ResolveObserver`

Monitors module resolution events. Useful for tracking which modules are imported.

```cpp
class MyObserver: public jsg::IsolateObserver {
public:
  kj::Own<ResolveStatus> onResolveModule(kj::StringPtr specifier,
                                          Context context,
                                          Source source) const override {
    KJ_LOG(INFO, "Resolving module", specifier,
           context == Context::BUNDLE ? "bundle" : "builtin",
           source == Source::STATIC_IMPORT ? "static" :
           source == Source::DYNAMIC_IMPORT ? "dynamic" :
           source == Source::REQUIRE ? "require" : "internal");

    // Return a ResolveStatus to track the resolution outcome
    return kj::heap<MyResolveStatus>(specifier);
  }
};

class MyResolveStatus: public jsg::ResolveObserver::ResolveStatus {
public:
  MyResolveStatus(kj::StringPtr specifier): specifier(kj::str(specifier)) {}

  void found() override {
    KJ_LOG(INFO, "Module found", specifier);
  }

  void notFound() override {
    KJ_LOG(WARNING, "Module not found", specifier);
  }

  void exception(kj::Exception&& e) override {
    KJ_LOG(ERROR, "Module resolution error", specifier, e.getDescription());
  }

private:
  kj::String specifier;
};
```

The `Context` enum indicates what is performing the resolution:
- `Context::BUNDLE` - User code from the worker bundle
- `Context::BUILTIN` - Built-in runtime modules
- `Context::BUILTIN_ONLY` - Internal modules only resolvable from builtins (e.g., `node-internal:*`)

The `Source` enum indicates how the resolution was triggered:
- `Source::STATIC_IMPORT` - Static `import` statement
- `Source::DYNAMIC_IMPORT` - Dynamic `import()` expression
- `Source::REQUIRE` - CommonJS `require()` call
- `Source::INTERNAL` - Internal module registry call

### `InternalExceptionObserver`

Monitors internal exceptions for metrics and debugging:

```cpp
class MyObserver: public jsg::IsolateObserver {
public:
  void reportInternalException(const kj::Exception& exception,
                               Detail detail) override {
    if (detail.isInternal) {
      KJ_LOG(ERROR, "Internal exception", exception.getDescription());
    }

    if (detail.isFromRemote) {
      KJ_LOG(INFO, "Exception from remote source");
    }

    if (detail.isDurableObjectReset) {
      KJ_LOG(INFO, "Durable Object reset");
    }

    KJ_IF_SOME(errorId, detail.internalErrorId) {
      KJ_LOG(INFO, "Error ID", kj::StringPtr(errorId.begin(), errorId.size()));
    }
  }
};
```

The `Detail` struct provides context about the exception:
- `isInternal` - True if this is an internal runtime error
- `isFromRemote` - True if the exception originated from a remote source
- `isDurableObjectReset` - True if this is a Durable Object reset
- `internalErrorId` - Optional unique identifier for the error

## BufferSource and BackingStore

JSG provides `BufferSource` and `BackingStore` types for working with binary data (ArrayBuffers
and TypedArrays). These types provide safe, efficient handling of buffer data with proper
integration with V8's memory management and sandbox.

### `jsg::BackingStore`

`BackingStore` wraps a `v8::BackingStore` and retains type information (byte length, offset,
element size) needed to recreate the original ArrayBuffer or TypedArray view.

**Key feature:** Once allocated, a `BackingStore` can be safely used outside the isolate lock.

#### Creating a BackingStore

```cpp
void createBackingStores(jsg::Lock& js) {
  // From a kj::Array (takes ownership)
  kj::Array<kj::byte> data = kj::heapArray<kj::byte>(1024);
  auto backing1 = jsg::BackingStore::from<v8::Uint8Array>(js, kj::mv(data));

  // Allocate a new zero-initialized buffer
  auto backing2 = jsg::BackingStore::alloc<v8::Uint8Array>(js, 1024);

  // Wrap external data with custom disposer
  void* externalData = /* ... */;
  auto backing3 = jsg::BackingStore::wrap<v8::Uint8Array>(
      externalData, 1024,
      [](void* data, size_t len, void* ctx) {
        // Custom cleanup when backing store is destroyed
        free(data);
      },
      nullptr  // context pointer passed to disposer
  );
}
```

The template parameter specifies the TypedArray type to create when converting back to JavaScript:
- `v8::ArrayBuffer` - Raw ArrayBuffer
- `v8::Uint8Array`, `v8::Int8Array`, `v8::Uint8ClampedArray`
- `v8::Uint16Array`, `v8::Int16Array`
- `v8::Uint32Array`, `v8::Int32Array`
- `v8::Float32Array`, `v8::Float64Array`
- `v8::BigInt64Array`, `v8::BigUint64Array`
- `v8::DataView`

#### Accessing Data

```cpp
void accessData(jsg::BackingStore& backing) {
  // Get as kj::ArrayPtr<byte>
  kj::ArrayPtr<kj::byte> bytes = backing.asArrayPtr();

  // Or with a different element type
  kj::ArrayPtr<uint32_t> u32s = backing.asArrayPtr<uint32_t>();

  // Implicit conversion also works
  kj::ArrayPtr<kj::byte> bytes2 = backing;

  // Query properties
  size_t size = backing.size();           // Byte length
  size_t offset = backing.getOffset();    // Byte offset into underlying buffer
  size_t elemSize = backing.getElementSize();  // Element size (1 for Uint8Array, 4 for Uint32Array, etc.)
  bool isInt = backing.isIntegerType();   // True for integer typed arrays
}
```

#### Manipulating Views

```cpp
void manipulateViews(jsg::Lock& js, jsg::BackingStore& backing) {
  // Create a typed view over the same underlying data
  auto uint16View = backing.getTypedView<v8::Uint16Array>();

  // Create a slice (shares underlying buffer)
  auto slice = backing.getTypedViewSlice<v8::Uint8Array>(10, 100);  // bytes 10-99

  // Consume bytes from the front (useful for streaming)
  backing.consume(10);  // Skip first 10 bytes

  // Trim bytes from the end
  backing.trim(10);     // Remove last 10 bytes

  // Limit to specific size
  backing.limit(100);   // Cap at 100 bytes

  // Clone (shares underlying buffer, but independent view)
  auto cloned = backing.clone();

  // Copy (creates new buffer with copied data)
  auto copied = backing.copy<v8::Uint8Array>(js);
}
```

#### Converting Back to JavaScript

```cpp
void convertToJs(jsg::Lock& js, jsg::BackingStore& backing) {
  // Create a JavaScript handle (ArrayBuffer or TypedArray based on template type)
  v8::Local<v8::Value> handle = backing.createHandle(js);
}
```

### `jsg::BufferSource`

`BufferSource` wraps a JavaScript ArrayBuffer or ArrayBufferView and provides the ability to
detach the backing store. It maintains a reference to the original JavaScript object.

The name "BufferSource" comes from the Web IDL specification.

#### Using BufferSource in API Methods

```cpp
class MyApi: public jsg::Object {
public:
  jsg::BufferSource processData(jsg::Lock& js, jsg::BufferSource source) {
    // Access data while still attached
    kj::ArrayPtr<kj::byte> data = source.asArrayPtr();
    // ... read data ...

    // Or detach to take ownership of the backing store
    jsg::BackingStore backing = source.detach(js);

    // Process the data
    kj::ArrayPtr<kj::byte> ptr = backing.asArrayPtr();
    for (auto& byte : ptr) {
      byte ^= 0xFF;  // Example: invert all bytes
    }

    // Return a new BufferSource with the modified data
    return jsg::BufferSource(js, kj::mv(backing));
  }

  JSG_RESOURCE_TYPE(MyApi) {
    JSG_METHOD(processData);
  }
};
```

#### Creating BufferSource

```cpp
void createBufferSource(jsg::Lock& js) {
  // From a BackingStore
  auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, 1024);
  jsg::BufferSource source1(js, kj::mv(backing));

  // From a JavaScript handle
  v8::Local<v8::Value> jsValue = /* ... */;
  jsg::BufferSource source2(js, jsValue);

  // Allocate directly (may return kj::none on allocation failure)
  KJ_IF_SOME(source3, jsg::BufferSource::tryAlloc(js, 1024)) {
    // Use source3...
  }

  // Wrap external data
  auto source4 = jsg::BufferSource::wrap(js, externalPtr, size, disposer, context);

  // From kj::Array using Lock helper
  kj::Array<kj::byte> data = kj::heapArray<kj::byte>(100);
  jsg::BufferSource source5 = js.arrayBuffer(kj::mv(data));
}
```

#### Detaching

Detaching removes the backing store from the BufferSource and neuters the JavaScript
ArrayBuffer/ArrayBufferView:

```cpp
void detachExample(jsg::Lock& js, jsg::BufferSource& source) {
  // Check if already detached
  if (source.isDetached()) {
    // Cannot access data
    return;
  }

  // Check if detachable (some ArrayBuffers cannot be detached)
  if (!source.canDetach(js)) {
    JSG_FAIL_REQUIRE(TypeError, "Buffer cannot be detached");
  }

  // Detach and take ownership
  jsg::BackingStore backing = source.detach(js);

  // The original JavaScript ArrayBuffer is now neutered (zero-length)
  // source.isDetached() is now true
}
```

#### Detach Keys

Some ArrayBuffers use detach keys for security:

```cpp
void detachWithKey(jsg::Lock& js, jsg::BufferSource& source) {
  // Set a detach key
  v8::Local<v8::Value> key = js.str("secret-key");
  source.setDetachKey(js, key);

  // Must provide the key to detach
  jsg::BackingStore backing = source.detach(js, key);
}
```

#### Other Operations

```cpp
void otherOps(jsg::Lock& js, jsg::BufferSource& source) {
  // Get the JavaScript handle
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

  // Trim the view
  source.trim(js, 10);  // Remove last 10 bytes

  // Clone (shares backing, new JS handle)
  jsg::BufferSource cloned = source.clone(js);

  // Copy (new backing, new JS handle)
  jsg::BufferSource copied = source.copy<v8::Uint8Array>(js);

  // Get a typed slice
  jsg::BufferSource slice = source.getTypedViewSlice<v8::Uint8Array>(js, 0, 100);

  // Zero the buffer
  source.setToZero();
}
```

#### GC Visitation

When storing a `BufferSource` as a member variable, you must visit it for GC:

```cpp
class MyClass: public jsg::Object {
public:
  void setBuffer(jsg::Lock& js, jsg::BufferSource buffer) {
    this->buffer = kj::mv(buffer);
  }

  JSG_RESOURCE_TYPE(MyClass) {
    JSG_METHOD(setBuffer);
  }

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

When V8 sandboxing is enabled, buffer memory must reside within the sandbox:

- `BackingStore::from()` will copy data if the source array is outside the sandbox
- `BackingStore::alloc()` always allocates inside the sandbox
- A `BackingStore` cannot be passed to another isolate unless both are in the same `IsolateGroup`

## V8 Platform Wrapper

JSG provides `V8PlatformWrapper` in `v8-platform-wrapper.h`, a wrapper around V8's platform
interface that allows JSG to intercept and customize platform operations.

### Purpose

The wrapper delegates most operations to an inner `v8::Platform` but wraps `JobTask` objects
to provide additional functionality. This is primarily used for:

- Monitoring background work scheduled by V8
- Ensuring proper KJ event loop integration
- Debugging and profiling V8's background tasks

### Implementation

```cpp
class V8PlatformWrapper: public v8::Platform {
public:
  explicit V8PlatformWrapper(v8::Platform& inner);

  // All Platform methods delegate to inner, except:
  // - CreateJobImpl wraps the JobTask to add custom behavior

  // ... delegates to inner platform ...
};
```

Most users won't interact with `V8PlatformWrapper` directly - it's used internally by
`V8System` to set up the V8 platform. However, understanding its existence helps when
debugging V8 background task issues or implementing custom platform behavior.

## Module System

JSG provides a module system that supports ES modules (ESM), CommonJS-style modules, and
various synthetic module types (JSON, WASM, data, text). There are two implementations:
the original `ModuleRegistry` (`modules.h`) and a newer, more flexible implementation
(`modules-new.h`).

### Concepts

**Module Types:**
- `Type::BUNDLE` - Modules from the worker bundle (user code)
- `Type::BUILTIN` - Built-in runtime modules (e.g., `node:buffer`, `cloudflare:sockets`)
- `Type::INTERNAL` - Internal modules only importable by other built-ins (e.g., `node-internal:*`)

**Resolution Priority:**
1. When importing from bundle code: Bundle → Builtin → Fallback
2. When importing from builtin code: Builtin → Internal
3. When importing from internal code: Internal only

**Module Info Types:**
- ESM modules - Standard JavaScript modules compiled by V8
- `CommonJsModuleInfo` - CommonJS-style modules with `require`/`exports`
- `DataModuleInfo` - Binary data as ArrayBuffer
- `TextModuleInfo` - Text content as string
- `WasmModuleInfo` - WebAssembly modules
- `JsonModuleInfo` - Parsed JSON data
- `ObjectModuleInfo` - JSG C++ objects exposed as modules
- `CapnpModuleInfo` - Cap'n Proto schema modules

### Original Module Registry (`modules.h`)

The original implementation provides a straightforward module registry:

```cpp
#include <workerd/jsg/modules.h>

// Install a module registry in a context
auto registry = jsg::ModuleRegistryImpl<MyTypeWrapper>::install(
    isolate, context, observer);

// Add a worker bundle module (ESM)
kj::Path specifier = kj::Path::parse("worker.js");
auto moduleInfo = jsg::ModuleRegistry::ModuleInfo(js,
    "worker.js",
    sourceCode,
    {},  // compile cache
    jsg::ModuleInfoCompileOption::BUNDLE,
    observer);
registry->add(specifier, kj::mv(moduleInfo));

// Add a built-in ESM module
registry->addBuiltinModule("node:buffer", sourceCode, jsg::ModuleRegistry::Type::BUILTIN);

// Add a built-in module from a bundle (capnp)
registry->addBuiltinBundle(bundle);

// Add a module backed by a C++ object
registry->addBuiltinModule<MyApiClass>("workerd:my-api");

// Add a module backed by an existing Ref
registry->addBuiltinModule("workerd:instance", kj::mv(myRef));

// Add a module with a factory callback
registry->addBuiltinModule("workerd:dynamic",
    [](jsg::Lock& js, auto method, auto& referrer) -> kj::Maybe<ModuleInfo> {
      // Lazily create the module
      return ModuleInfo(js, "workerd:dynamic", kj::none,
          ObjectModuleInfo(js, createMyObject(js)));
    });
```

#### Resolving Modules

```cpp
// Get the registry from the current context
auto* registry = jsg::ModuleRegistry::from(js);

// Resolve a module by specifier
kj::Path specifier = kj::Path::parse("./utils.js");
kj::Path referrer = kj::Path::parse("worker.js");

KJ_IF_SOME(info, registry->resolve(js, specifier, referrer)) {
  // Module found, get the V8 module handle
  v8::Local<v8::Module> module = info.module.getHandle(js);

  // Instantiate and evaluate
  jsg::instantiateModule(js, module);

  // Get the module namespace
  v8::Local<v8::Value> ns = module->GetModuleNamespace();
}

// Resolve options
registry->resolve(js, specifier, referrer,
    ModuleRegistry::ResolveOption::BUILTIN_ONLY);  // Only search builtins
registry->resolve(js, specifier, referrer,
    ModuleRegistry::ResolveOption::INTERNAL_ONLY); // Only search internals
```

#### Creating Synthetic Modules

```cpp
// Data module (ArrayBuffer)
kj::Array<kj::byte> data = loadBinaryData();
v8::Local<v8::ArrayBuffer> buffer = js.wrapBytes(kj::mv(data));
auto info = ModuleInfo(js, "data.bin", kj::none,
    DataModuleInfo(js, buffer));

// Text module
v8::Local<v8::String> text = js.str("Hello, World!");
auto info = ModuleInfo(js, "text.txt", kj::none,
    TextModuleInfo(js, text));

// JSON module
auto jsonValue = jsg::check(v8::JSON::Parse(context, js.str(jsonContent)));
auto info = ModuleInfo(js, "config.json", kj::none,
    JsonModuleInfo(js, jsonValue));

// WASM module
auto wasmModule = jsg::compileWasmModule(js, wasmBytes, observer);
auto info = ModuleInfo(js, "module.wasm", kj::none,
    WasmModuleInfo(js, wasmModule));
```

#### CommonJS Modules

```cpp
// Create a CommonJS module provider
class MyModuleProvider: public ModuleRegistry::CommonJsModuleInfo::CommonJsModuleProvider {
public:
  JsObject getContext(Lock& js) override {
    // Return the object to use as 'this' context
    return js.obj();
  }

  JsValue getExports(Lock& js) override {
    // Return the initial exports object
    return js.obj();
  }
};

auto provider = kj::heap<MyModuleProvider>();
auto info = ModuleRegistry::CommonJsModuleInfo(js, "module.js", sourceCode, kj::mv(provider));
```

### New Module Registry (`modules-new.h`)

The new implementation provides better modularity, URL-based specifiers, thread safety,
and support for module sharing across isolate replicas.

#### Key Improvements

1. **URL-based specifiers** - All module specifiers are URLs
2. **Thread-safe** - Designed for sharing across isolate replicas
3. **Modular bundles** - Separate `ModuleBundle` instances for different module sources
4. **Better import.meta support** - `import.meta.url`, `import.meta.main`, `import.meta.resolve()`
5. **Import attributes** - Support for import attributes (assertions)

#### Creating Modules

```cpp
#include <workerd/jsg/modules-new.h>

using namespace workerd::jsg::modules;

// Create an ESM module (takes ownership of code)
kj::Array<const char> code = loadSourceCode();
auto esmModule = Module::newEsm(
    "file:///bundle/worker.js"_url,
    Module::Type::BUNDLE,
    kj::mv(code),
    Module::Flags::MAIN | Module::Flags::ESM);

// Create an ESM module (references static code)
auto builtinModule = Module::newEsm(
    "node:buffer"_url,
    Module::Type::BUILTIN,
    STATIC_SOURCE_CODE);

// Create a synthetic module
auto syntheticModule = Module::newSynthetic(
    "workerd:my-module"_url,
    Module::Type::BUILTIN,
    [](Lock& js, const Url& id, const Module::ModuleNamespace& ns,
       const CompilationObserver& observer) -> bool {
      // Set exports
      ns.setDefault(js, js.obj());
      ns.set(js, "foo", js.str("bar"));
      return true;
    },
    kj::arr("foo"_kj));  // Named exports
```

#### Module Handlers for Common Types

```cpp
// Text module handler
auto textHandler = Module::newTextModuleHandler(textContent);
auto textModule = Module::newSynthetic(id, type, kj::mv(textHandler));

// Data module handler (ArrayBuffer)
auto dataHandler = Module::newDataModuleHandler(binaryData);
auto dataModule = Module::newSynthetic(id, type, kj::mv(dataHandler));

// JSON module handler
auto jsonHandler = Module::newJsonModuleHandler(jsonContent);
auto jsonModule = Module::newSynthetic(id, type, kj::mv(jsonHandler));

// WASM module handler
auto wasmHandler = Module::newWasmModuleHandler(wasmBytes);
auto wasmModule = Module::newSynthetic(id, type, kj::mv(wasmHandler),
    nullptr, Module::Flags::WASM);
```

#### Building Module Bundles

```cpp
// Build a worker bundle
Url bundleBase = "file:///bundle"_url;
ModuleBundle::BundleBuilder bundleBuilder(bundleBase);

bundleBuilder
    .addEsmModule("worker.js", workerSource, Module::Flags::MAIN | Module::Flags::ESM)
    .addEsmModule("utils.js", utilsSource)
    .addSyntheticModule("config", configHandler)
    .alias("./lib", "./utils.js");

auto workerBundle = bundleBuilder.finish();

// Build a builtin bundle
ModuleBundle::BuiltinBuilder builtinBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN);

builtinBuilder
    .addEsm("node:buffer"_url, bufferSource)
    .addEsm("node:path"_url, pathSource)
    .addSynthetic("cloudflare:sockets"_url, socketsHandler);

// Add a module backed by a C++ object
builtinBuilder.addObject<MyApiClass, MyTypeWrapper>("workerd:my-api"_url);

auto builtinBundle = builtinBuilder.finish();

// Build an internal-only bundle
ModuleBundle::BuiltinBuilder internalBuilder(ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

internalBuilder
    .addEsm("node-internal:primordials"_url, primordialsSource)
    .addEsm("node-internal:errors"_url, errorsSource);

auto internalBundle = internalBuilder.finish();
```

#### CommonJS-Style Modules

```cpp
// The template type must be a jsg::Object with getExports(Lock&) method
template <typename TypeWrapper>
auto handler = Module::newCjsStyleModuleHandler<NodeFsContext, TypeWrapper>(
    sourceCode, "node:fs");

builtinBuilder.addSynthetic("node:fs"_url, kj::mv(handler));
```

#### Fallback Bundle (for local development)

```cpp
// Create a fallback bundle that dynamically resolves modules
auto fallbackBundle = ModuleBundle::newFallbackBundle(
    [](const ResolveContext& context)
        -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
      // Try to load the module from external source
      KJ_IF_SOME(code, fetchModule(context.normalizedSpecifier)) {
        return Module::newEsm(
            context.normalizedSpecifier.clone(),
            Module::Type::FALLBACK,
            kj::mv(code));
      }

      // Or return a redirect
      if (shouldRedirect(context.normalizedSpecifier)) {
        return kj::str("node:buffer");  // Redirect to another module
      }

      return kj::none;  // Module not found
    });
```

#### Resolution Context

The `ResolveContext` provides information about module resolution:

```cpp
struct ResolveContext {
  Type type;           // BUNDLE, BUILTIN, or BUILTIN_ONLY
  Source source;       // STATIC_IMPORT, DYNAMIC_IMPORT, REQUIRE, or INTERNAL

  const Url& normalizedSpecifier;        // Fully resolved URL
  const Url& referrerNormalizedSpecifier; // URL of importing module

  kj::Maybe<kj::StringPtr> rawSpecifier;  // Original specifier before normalization

  // Import attributes (e.g., { type: "json" })
  kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
};
```

### Module Instantiation

Both implementations use similar instantiation:

```cpp
// Get the V8 module
v8::Local<v8::Module> module = moduleInfo.module.getHandle(js);

// Instantiate (resolves imports, links modules)
jsg::instantiateModule(js, module);

// Or with options
jsg::instantiateModule(js, module, InstantiateModuleOptions::NO_TOP_LEVEL_AWAIT);

// Get the module namespace (exports)
v8::Local<v8::Value> ns = module->GetModuleNamespace();
```

### Dynamic Import

Dynamic imports are handled through V8's callback mechanism:

```cpp
// The registry handles dynamic imports automatically
// In JavaScript:
const module = await import('./other.js');
const { foo } = await import('node:path');
```

### require() Support

For CommonJS compatibility:

```cpp
// Get exports using require semantics
JsValue exports = ModuleRegistry::requireImpl(js, moduleInfo);

// Or get just the default export
JsValue defaultExport = ModuleRegistry::requireImpl(js, moduleInfo,
    ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT);
```

### Choosing Between Implementations

Use the **original implementation** (`modules.h`) when:
- You need simpler, more direct control
- You're working with existing code that uses kj::Path specifiers
- Thread safety across replicas isn't needed

Use the **new implementation** (`modules-new.h`) when:
- You need URL-based module specifiers
- You need import.meta support
- You need to share modules across isolate replicas
- You need better import attribute handling
- You're building new code from scratch
