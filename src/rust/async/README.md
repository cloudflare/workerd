# KJ Promise / Rust Future Interoperability

This crate implements the ability to share two generic awaitable types between C++ and Rust: `BoxFuture<T>` and `kj::Promise<T>`.

These two generic types allow for C++ to own and poll Rust Futures, and for Rust to own and poll KJ Promises.

We also implement each language's await syntax for each generic type. In C++, `BoxFuture<T>` has a `co_await` operator that allows KJ coroutines to await Rust Futures. In Rust, `Promise<T>` has an `IntoFuture` trait implementation that allows `async` code blocks to `.await` KJ Promises.

## Boilerplate

cxx-rs does not currently support exposing arbitrary generic types across languages. This forces us to define type aliases, in both languages, for each concrete `BoxFuture<T>` and `Promise<T>` we want to share. For example:

```cpp
using BoxFutureFallibleI32 = BoxFuture<Fallible<int32_t>>;
using PromiseI32 = kj::Promise<int32_t>
```
```rust
type BoxFutureFallibleI32 = crate::BoxFuture<crate::Result<i32>>;
type PromiseI32 = crate::Promise<i32>;
```

Each such type alias requires additional boilerplate code to be defined. This additional code includes more type aliases, function template specializations in C++, trait implementations in Rust, and function definitions in both languages.

Boilerplate code currently lives in three locations: certain blocks within the `ffi` module in `lib.rs`, and the contents of all the files ending in `-boilerplate.h/c++` and `_boilerplate.rs`.

Currently, all the boilerplate is handwritten. This sucks, and needs to be improved. Likely the way to do this is with a proc macro in Rust, and a preprocessor macro in C++.

Once all the required boilerplate code is defined for a new Future type, such as `BoxFutureFallibleI32`, you can return one from Rust to C++ and `co_await` it from a KJ coroutine. Similarly, once all the required boilerplate code is defined for a new Promise type, such as `PromiseI32`, you can return one from C++ to Rust, and `.await` it from an `async` code block, _as long as the `async` block (Future) is being driven by the KJ runtime_. In practical terms, this means that Rust can currently await KJ Promises only in code that is itself awaited by a KJ coroutine.

## `BoxFuture<T>` in detail

Future is a trait in Rust, rather than a type. For example, each `async { ... }` block of code has its own generated type, similar to lambdas in C++, and this type has an associated Future trait implementation.

Trying to expose every possible type implementing the Future trait to C++ directly would generate more complexity than it is worth. Instead, we expose the type-erased [`dyn Future`](https://doc.rust-lang.org/std/keyword.dyn.html) type, at the cost of heap-allocating the Future in a Box.

In Rust, we define `BoxFuture<T>` as a tuple struct containing `Pin<Box<dyn Future<Output = T> + Send>>`.

### Nuances

[Pinning](https://doc.rust-lang.org/std/pin/index.html) the Future is required in order to poll it, so we require it already to be pinned when the `BoxFuture<T>` is created.

We require the Future to be `Send`, because C++ code generally assumes ownership of objects can migrate between threads.

`BoxFuture<T>` is essentially the same as [`futures::future::BoxFuture<T>`](https://docs.rs/futures/latest/futures/future/type.BoxFuture.html), with the difference being that ours is a tuple struct instead of a type alias. This is required because cxx-rs only allows structs defined in our own crate to be exposed to C++.

This mandatory heap allocation into a Box means that returning a Future from Rust to C++ requires it to be wrapped in some code like `Box::pin(future).into()`. This contrasts with returning KJ Promises from C++ to Rust, which are already appropriately type-erased and, typically, heap-allocated.

### Ownership in C++

The cxx-rs crate does not support exposing `Box<dyn Trait>` types directly to C++, because `dyn Trait` types do not implement Sized and Unpin. Thus, exposing the type the naive way would require an extra Box around the whole thing. The author of cxx-rs demonstrated a [technique to work around this limitation here](https://github.com/dtolnay/cxx/pull/672), which we use. Notably, the [`cxx-async`(https://docs.rs/cxx-async/latest/cxx_async/) crate [uses the same technique](https://github.com/pcwalton/cxx-async/blob/ac98030dd6e5090d227e7fadca13ec3e4b4e7be7/macro/src/lib.rs#L169).

In C++, we define a `BoxFuture<T>` class template with the same size and alignment as the identically-named generic tuple struct in Rust. Specifically, it contains two 64-bit words to match Rust's "fat pointer" layout: one pointer for the object, one for its vtable.

Owning an object in C++ means being able to destroy it. In Rust, this means running the Drop trait. Therefore, we need a way to run the Drop trait from C++. To do so, we define a `box_future_drop_in_place<T>()` function template in C++, and specialize it in boilerplate code for every `T` in `BoxFuture<T>` we want to support. The specializations of this function template in turn call `T`-specific boilerplate functions defined in Rust, with names like `box_future_drop_in_place_void()`, and exposed via our cxxbridge FFI module. Those functions in turn call the Future's Drop trait.

### Move semantics

`BoxFuture<T>` is moveable in Rust, and needs to be moveable in C++ as well to be useful. Fortunately, Rust move semantics are quite simple: objects are memcpyed, and the Drop trait is simply never run on the object's old location. In particular, there's no move constructor and move assignment operator special functions that need to be run, like in C++.

`BoxFuture<T>` emulates Rust's move semantics in C++ by zeroing out the old object's data words in the new object's move constructor, and later only calling `box_future_drop_in_place<T>()` in the destructor if those words are non-zero.

### Polling

To get a value `T` _out_ of a `BoxFuture<T>`, we need a way to call its `poll()` Future trait function. `poll()` is this little function:

```rust
    fn poll(self: Pin<&mut Self>, cx: &mut Context) -> Poll<T>;
```

- It accepts a `&mut std::task::Context` which wraps a `&std::task::Waker` (and nothing else).
- It returns either a `Poll::Ready(T)` or a `Poll::Pending` result.

We cannot directly express the `poll()` function's parameter type, nor its return type, in cxx-rs. The parameter type is fairly easily solvable, because `std::task::Waker` conveniently provides a low-level API for embedders to instantiate custom Waker implementations from raw pointers and a vtable.

The return type is more difficult, because cxx-rs does not support `Option<T>`, which would be the naive emulation for `Ready(T)`. We could pass a struct containing a boolean and a value of type `T`, but this would require `T` to be default-constructible, limiting the usefulness of our `BoxFuture<T>` type.

The solution in this crate is to define the Rust FFI function `box_future_poll_T()` like so:

```rust
pub fn box_future_poll_T(
    future: Pin<&mut BoxFutureT>,
    waker: &KjWaker,
    fulfiller: Pin<&mut BoxFutureFulfillerT>,
) -> bool {
```

There are three things to note:

- We pass a reference to a `KjWaker` C++ class. The implementation of `box_future_poll_T()` knows how to turn this reference into a `std::task::Waker` for the inner call to `poll()`.

- We pass a reference to a `BoxFutureFulfiller<T>` C++ class. This class has one function, `fulfill(T)` (or `fulfill()` if `T = void`). The implementation of `box_future_poll_T()` calls this function with the result of the Future iff the inner call to `poll()` returns `Ready(T)`.

- We return true if the Future is Ready, false if the Future is Pending. Invariant: returning true means we called `fulfill()` on the `BoxFutureFulfiller<T>`.

### Wakers

In Rust, awaiting a Future means to repeatedly call `poll()` with a Waker until the Future returns `Ready(T)`. Typically, `poll()` is only called after the implementation of the Future being polled arranges to call `wake()` on the Waker which was last passed to `poll()`.

In KJ terminology, a Waker is similar in concept to a `kj::PromiseFulfiller<void>`, except that dropping the last Waker clone does not reject anything. You can either wake a Waker, or not.

Rust imposes some further constraints on Wakers: they must be `Send` and `Sync` (or, I guess `&Waker` is the one that needs to be `Sync`). This means that `kj::CrossThreadPromiseFulfiller<void>` is the more appropriate KJ equivalent for Waker.

This crate defines several implementations of the `KjWaker` class, which we wrap in a Waker on the Rust side. Two major ones include:

- `ArcWaker` - A wrapper around a `kj::CrossThreadPromiseFulfiller<void>`. Constructed with `newPromiseAndArcWaker()`, which returns a pair of `kj::Promise<void>` and `kj::Arc<const ArcWaker>`.

- `LazyArcWaker` - A type intended to live immobile on the stack. When some borrower of the `&Waker` which wraps the `LazyArcWaker` attempts to clone the `Waker`, `LazyArcWaker` only then instantiates an `ArcWaker`. This allows us to avoid the extra overhead associated with cross-thread Promise fulfillment, and atomic-refcounting, in some situations.

### Await syntax

In order to repeatedly call `poll()` on the KJ event loop, we need a KJ Event whose `fire()` callback implementation knows the Future's Output type, `T`. We provide this in the form of two classes: `FuturePollEvent`, which serves as the base class for `BoxFutureAwaiter<T>`.

`FuturePollEvent` connects the KJ Event to the Promise associated with an ArcWaker, LazyArcWaker, or any number of Promises which the Rust Future may itself be polling with the same Waker. Once any of these Promises become ready, the `FuturePollEvent` is armed and fired on the KJ event loop.

`BoxFutureAwaiter<T>` provides the actual type-specific `fire()` callback implementation. It polls the Future, stores its result if it is Ready, and arms the coroutine if so.

`BoxFutureAwaiter<T>` polls the Future using a `FuturePollEvent::PollScope` as its Waker. `PollScope` is a combination of a `LazyArcWaker` and an implementation of a special function, `KjWaker::tryGetFuturePollEvent()`. The implementation of await syntax for KJ Promises in Rust uses this special function to connect Promises which they await using the same Waker directly to the FuturePollEvent.

### Fallibility

Rust conveys fallibility using `Result<T, E>`. If, in Rust, we have a `BoxFuture<Result<T, E>>`, then the corresponding boilerplate functions are infixed with `_fallible_`, such as `box_future_drop_in_place_fallible_T()`. Corresponding type aliases are infixed with `Fallible`, such as `BoxFutureFallibleT`.

Additionally, the `box_future_poll_fallible_T()` boilerplate function returns `Result<bool>` instead of just `bool`. This causes cxx-rs to throw an exception when we call that FFI function from C++.

Note that `box_future_poll_fallible_T()` still also accepts a type alias to a `BoxFutureFulfiller<Fallible<T>>`, which might suggest a second way to propagate an error. However, the interface of `BoxFutureFulfiller<Fallible<T>>` is identical to that of `BoxFutureFulfiller<T>`. That is, errors are always communicated via throwing from the FFI poll function.

### Required boilerplate

Futures require the following boilerplate in C++:

- A type alias of `BoxFuture<T>`, e.g. `BoxFutureI32`.
- An explicit specialization of `box_future_drop_in_place<T>(PtrBoxFuture<T>)`, which forwards to the Rust boilerplate function `box_future_drop_in_place_T()`.
- A type alias of `PtrBoxFuture<T>`, which is in turn an alias of `BoxFuture<T>*`.
- An explicit specialization of `box_future_poll<T>(BoxFuture<T>&, const KjWaker&, BoxFutureFulfiller<T>&)`, which forwards to the Rust boilerplate function `box_future_poll_T()`.
- A type alias of `BoxFutureFulfiller<T>`, with one member function `fulfill()`, accepting either nothing (if `T = void`) or a value of type `T`.

Futures additionally require the following boilerplate in Rust:

- Type aliases matching the boilerplate C++ type aliases inside of our cxxbridge `ffi` module.
- A declaration for the `BoxFutureFulfiller<T>::fulfill()` member function inside of our cxxbridge `ffi` module.
- A `box_future_drop_in_place_T(PtrBoxFuture<T>)` function to run the Future's Drop trait.
- A `box_future_poll_T(Pin<&mut BoxFuture<T>>, &KjWaker, Pin<&mut BoxFutureFulfiller<T>)` function to poll the Future.
- `cxx::ExternType` trait implementations for `BoxFuture<T>` and `PtrBoxFuture<T>`.

## `Promise<T>` in detail

In C++, a `kj::Promise<T>` contains (and can be converted to) a type-erased `OwnPromiseNode`. Working with this type-erased type is more convenient in the implementation of `Promise<T>`'s IntoFuture trait, because it reduces the amount of boilerplate we must generate.

We therefore allow Rust to own a KJ Promise in one of two forms: as an actual `Promise<T>`, or as an `OwnPromiseNode`. The `OwnPromiseNode` form is only intended to be used in this crate's implementation; users are expected primarily to use `Promise<T>`.

In both cases, `Promise<T>` and `OwnPromiseNode`, we define their Rust equivalent structs as containing a single 64-bit pointer, which matches their C++ layout.

### Ownership

As in C++, owning an object in Rust means being able to drop it. Dropping something in C++ means running its destructor, so we need a way to run `Promise<T>`'s and `OwnPromiseNode`'s C++ destructors from Rust.

In `OwnPromiseNode`'s case, we define a free function, `own_promise_node_drop_in_place()` which simply runs `OwnPromiseNode`'s destructor.

For `Promise<T>`, we must find a way to dispatch from a generic type's implementation to a type-specific boilerplate function defined in C++.

When going the opposite direction, we used explicit function template specialization of `box_future_drop_in_place<T>()` to solve this problem. There is no such thing as explicit generic specialization in Rust, however. Instead, we copy the trick used by the cxx-rs crate: we define a `PromiseTarget` trait with associated functions which forward to our C++ boilerplate functions, including `promise_drop_in_place_T()`, then implement that trait for each `T` in `Promise<T>` that we want to await in Rust. `Promise<T>`'s Drop trait implementation thus forwards its call to `T::drop_in_place(promise)`, which forwards to the C++ boilerplate function, which calls the actual destructor.

### Move semantics

Since `OwnPromiseNode` and `kj::Promise<void>` move constructors leave their old objects nullified in such a way that their destructors are no-ops, both types are compatible with Rust move semantics. Rust is free to move them by memcpy, and does not need to run any C++ special function to complete the move, because Rust does not run Drop traits on old object locations.

### Await syntax

In Rust, `<expr>.await` is [syntax sugar for something like this](https://github.com/rust-lang/rust/blob/8a8b4641754e9ce8a31b272dda6567727452df9e/compiler/rustc_ast_lowering/src/expr.rs#L820-L834):

```rust
match ::std::future::IntoFuture::into_future(<expr>) {
    mut __awaitee => loop {
        match unsafe { ::std::future::Future::poll(
            <::std::pin::Pin>::new_unchecked(&mut __awaitee),
            ::std::future::get_context(task_context),
        ) } {
            ::std::task::Poll::Ready(result) => break result,
            ::std::task::Poll::Pending => {}
        }
        task_context = yield ();
    }
}
```

In order to support `.await` for KJ Promises, then, we must implement the ability to poll KJ Promises, with the help of the `IntoFuture` trait's `into_future()` associated function.

Instead of a `poll()` function, the `OwnPromiseNode` inside of KJ Promises exposes two functions:

- `onReady(kj::_::Event*)`
- `get(kj::_::ExceptionOrValue&)`

Teaching Rust to work with `kj::_::Event` and `kj::_::ExceptionOrValue` types would not be particularly easy. Instead, we can make the result of `IntoFuture::into_future()` own a C++ object which helps implement `poll()` in terms of PromiseNode's interface.

### Polling

The result of `IntoFuture::into_future(promise)` is the horribly-named `PromiseFuture<T>`. This `PromiseFuture<T>` is implemented in Rust, but contains a C++ class inside of it, `RustPromiseAwaiter`, which ends up owning the `OwnPromiseNode` for awaiting.

`RustPromiseAwaiter` implements the `kj::_::Event` interface, and, for every call to `PromiseFuture::poll()`, `RustPromiseAwaiter` arranges for the PromiseNode to arm the `RustPromiseAwaiter` Event when it becomes ready, using `PromiseNode::onReady()`.

When the PromiseNode becomes ready, `RustPromiseAwaiter`'s `fire()` callback records the fact that the Promise is ready and wakes whatever Waker it was last polled with. The next call to `PromiseFuture::poll()` observes that the Promise is ready, extracts the `OwnPromiseNode` from the `RustPromiseAwaiter`, then calls `PromiseNode::get()` to extract the actual value using the C++ `own_promise_node_unwrap_T()` boilerplate function.

### Fallibility

Unlike Rust Futures, which have fallibility baked into their types, all Promises are fallible. This means that the `own_promise_node_unwrap_T()` boilerplate function may always throw in C++, and in Rust is defined to return `std::result::Result<T, cxx::Exception>`.

### Required boilerplate

Promises require the following C++ boilerplate:

- The `Promise<T>` type alias, e.g. `PromiseI32`.
- A `promise_drop_in_place_T(PtrPromise<T>)` function which runs `Promise<T>`'s destructor.
- A type alias for `PtrPromise<T>`, which is in turn an alias for `Promise<T>*`.
- A `promise_into_own_promise_node_T(Promise<T>) -> OwnPromiseNode` function to extract the PromiseNode from a type-specific Promise for awaiting.
- An `own_promise_node_unwrap_T(OwnPromiseNode)` function to extract a value T from the awaited OwnPromiseNode.

Promises additionally require the following Rust boilerplate:

- Definitions matching all of the C++ boilerplate inside of our cxxbridge `ffi` module.
- A `PromiseTarget` trait implementation for `T`. The `PromiseTarget` trait consists of three functions which forward to the three C++ boilerplate functions associated with `Promise<T>`.
- `cxx::ExternType` trait implementations for `Promise<T>` and `PtrPromise<T>`.

# TODO

Expand on these topics:
- thread safety
- lifetime safety
- pin safety
- usage of bindgen
- LazyPinInit
- ExecutorGuarded
- overhead
