# What is JSG?

`jsg` is an abstraction API we use to hide many of the complexities of translating back and
forth between JavaScript and C++ types. Think of it as a bridge layer between the kj-based
internals of the runtime and the v8-based JavaScript layer.

Ideally, JSG would be a complete wrapper around V8, such that code using JSG does not need to
interact with V8 APIs at all. Perhaps, then, different implementations of JSG could provide
the same interface on top of different JavaScript engines. However, as of today, this is not
quite the case. At present application code will still need to use V8 APIs directly in some
cases. We would like to improve this in the future.

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
const Foo: public jsg::Object {
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
| jsg::ByteString      |  v8::String    |     string      | See [ByteString][] spec |
| kj::Date             |  v8::Date      |     Date        |                         |
| nullptr              |  v8::Null      |     null        | See kj::Maybe&lt;T>     |
| nullptr              |  v8::Undefined |     undefined   | See jsg::Optional&lt;T> |

Specifically, for example, when mapping from JavaScript into C++, when JSG encounters a
string value, it can convert that into either a `kj::String`, or `jsg::ByteString`,
depending on what is needed by the C++ layer. Likewise, when translating from C++ to
JavaScript, JSG will generate a JavaScript `string` whenever it encounters a `kj::String`,
`kj::StringPtr`, or `jsg::ByteString`.

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

TODO(soon): Open Question: What happens if you try `kj::OneOf<kj::Maybe<kj::String>,
kj::Maybe<bool>>` and pass a `null`?

TODO(soon): Open Question: What happens if you have a catch-all coercible like kj::String
in the list?

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

```cpp
jsg::Lock& js = // ...
jsg::Promise<int> promise = // ...

promise.then(js, [&](jsg::Lock& js, int value) {
  // Do something with value.
}, [&](jsg::Lock& js, jsg::Value exception) {
  // There was an error! Handle it!
})
```

TODO(soon): Lots more to add here.

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
// We'll discuss jsg::alloc a bit later as part of the introduction to Resource Types.
jsg::Ref<Foo> foo = jsg::alloc<Foo>();

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
    return jsg::alloc<Foo>(value);
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

TODO(soon): TBD

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

## Utilities

TODO(soon): TBD

### Errors

TODO(soon): TBD

* `makeInternalError(...)`
* `throwInternalError(...)`
* `throwTypeError(...)`
* //...

#### `jsg::DOMException`

TODO(soon): TBD

#### Tunneled Exceptions

TODO(soon): TBD

### `jsg::v8Str(...)` and `jsg::v8StrIntern(...)`

TODO(soon): TBD

["KJ Style Guide"]: https://github.com/capnproto/capnproto/blob/master/style-guide.md
["KJ Tour"]: https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md
[ByteString]: https://webidl.spec.whatwg.org/#idl-ByteString
[Record]: https://webidl.spec.whatwg.org/#idl-record
[Sequence]: https://webidl.spec.whatwg.org/#idl-sequence

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

2. Otherwise, the override will be converted to a TypeScript class as follows: (where `<name>` is the
   unqualified C++ type name of this resource type)

   1. If an override starts with `extends `, `implements ` or `{`: `class <name> ` will be prepended
   2. If an override starts with `<`: `class <name>` will be prepended
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
