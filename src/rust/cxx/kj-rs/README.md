# KJ integration for the in-tree CXX fork

This directory contains the Rust and C++ support for passing KJ types across CXX bridges. It is
built as `//src/rust/cxx/kj-rs` and consumed directly by workerd's Rust crates.

It provides exception conversion, KJ ownership and refcount wrappers, `kj::Maybe` and `kj::Date`
interop, and adapters between Rust futures and `kj::Promise`. The code moved into workerd together
with the rest of the former `workerd-cxx` repository; it is no longer fetched as an external Bazel
dependency.

## Dropping a `KjOwn<T>`

`KjOwn<T>` mirrors `kj::Own<T>`'s two-word layout, and dropping it runs `kj::Own<T>::~Own()` in
C++. That destructor is typed: for a polymorphic `T` it hands the disposer
`dynamic_cast<void*>(ptr)`, the complete object's address, which differs from the `T*` when `T`
is a base at a nonzero offset of the object it points into (multiple inheritance). Rust cannot
compute that offset, so `KjOwn<T>` requires `T: kj_rs::OwnTarget`, and `OwnTarget::__drop`
calls a C++ function generated for that `T`:

```
void cxxbridge$kjrs$own$<namespace>$<Type>$drop(::kj::Own<T>* own) { own->~Own(); }
```

A bridge generates the `OwnTarget` implementation and this function for each `T` it declares
(`type T;`) and holds in a `KjOwn<T>`, next to its other per-type instantiations
(`UniquePtr<T>`, `KjMaybe<T>`). A bridge that only aliases `T` (`type T = other::ffi::T;`)
cannot implement the trait for it, so the declaring bridge must supply it: either by naming
`KjOwn<T>` itself or with an explicit `impl KjOwn<T> {}`. The primitive types a `KjOwn` may
hold are implemented in `own.rs` over one untyped drop in `own.c++`: a primitive is never
polymorphic, so its `kj::Own` passes the disposer the pointer unchanged. The generated C++ asserts that
`T` is complete where the bridge is compiled, since `~Own()` needs the definition.

Tests live in [`tests/`](tests/) and run as part of:

```sh
bazel test //src/rust/cxx/...
```

## Event-loop-bound values in `Send` futures

An embedder may run a KJ event loop, with its tasks, on different OS threads between turns, so
`IoContext::await_io` and `IoContext::await_io_and_run` require `Send` futures and callbacks to
rule out OS-thread-pinned state (`MutexGuard`, `Rc` cloned from a `thread_local!`, etc.). The
futures of bridged C++ promises are `!Send`, as is a `KjOwn<T>` unless `T` is `Send`, which an
opaque C++ type is not by default, so they cannot accidentally cross into `tokio::spawn` or JSG
resources; two opt-in wrappers bridge them into `Send` futures that stay on the same KJ event loop:

- **`KjFuture::into_send()`** — for bridged C++ `async fn` calls. Every `extern "C++"` `async fn`
  requires a current KJ event loop and returns a `!Send`, `!Unpin` `kj_rs::KjFuture` carrying that
  loop's `kj::EventLoop::Id`. Call `.into_send()` to get a `Send`, `!Sync`, `!Unpin`
  `SendKjFuture` that preserves borrowed arguments, returns an error if polled off its loop, and
  aborts the process if dropped off its loop (including after the loop is destroyed).
- **`LoopChecked<T>`** — for non-future values that belong to a KJ event loop rather than an
  OS thread: KJ objects, or Rust state such as an `Rc` whose clones all belong to the loop. It
  records `kj::EventLoop::Id::current()` at construction, is `Send` and `!Sync`, fails
  `get()`/`get_mut()`/`get_pin_mut()` off its loop, panics if polled as a `Future` off its loop,
  and aborts if dropped off its loop or inside `kj::DisallowAsyncDestructorsScope`.
  - **Wrap at the binding's construction site, inside a private field** (as `io_context::SpanBuilder`
    and `io_context::TraceContext` do). That binds the object to its real loop at birth, keeps
    `unsafe` out of callers, keeps code that could store a different value through `get_mut()`
    out of reach, and avoids leaking `LoopChecked<T>` into public signatures.
  - **Keep loop-bound wrappers `!Traced`** so `#[jsg_resource]` and `#[jsg_traced]` reject them at
    compile time (and `DisallowAsyncDestructorsScope` catches manual `Traced` bypasses at GC).

### When to use what

| Where the KJ value lives | What to use |
| --- | --- |
| Single-loop task (`kj_rs_tokio::spawn` / `LocalSet`, `server/`) | Plain `!Send` `KjFuture` and `KjOwn<T>` directly (no wrapper needed) |
| `IoContext::await_io` / `await_io_and_run` future or callback | `KjFuture::into_send()` for C++ `async fn`; `LoopChecked<KjOwn<T>>` inside the binding's wrapper struct for KJ objects |
| JSG resource (`#[jsg_resource]`) or promise reaction | `IoOwned<T>` (`IoContext::add_object` / `add_span`), never a bare `LoopChecked` or `SendKjFuture` |
| `tokio::spawn` (multi-threaded or outliving the KJ loop) | Do **not** use `SendKjFuture` or `LoopChecked` — dropping them off-loop aborts |
