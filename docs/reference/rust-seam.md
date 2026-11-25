# The Rust seam

The runtime reaches whoever embeds it through a set of interfaces: the _Rust seam_. On the runtime
side are `WorkerEntrypoint`, `IoContext` and the APIs. They are moving to Rust. On the embedder side
are the workerd server and other embedders. They can stay C++, and they keep compiling and working
unchanged while the runtime side moves.

```
  embedder (C++ or Rust)  |  seam interfaces   |  runtime (moving to Rust)
                          |                    |
  workerd server,         |  requests in  ---> |  WorkerEntrypoint,
  other embedders         |  <--- calls out    |  IoContext, APIs
```

Two directions cross the seam:

- **Requests in.** An embedder delivers events to a Worker through C++ `WorkerInterface`
  (`src/workerd/io/worker-interface.h`) or its Rust counterpart, `worker::Interface`
  (`src/rust/worker/`, see its [`AGENTS.md`](../../src/rust/worker/AGENTS.md)). On the runtime side,
  C++ `WorkerEntrypoint` receives them and hands the events that Rust delivers to the `entrypoint`
  crate (`src/workerd/io/entrypoint/`).
- **Calls out.** The runtime calls its host through `IoChannelFactory`, `LimitEnforcer`,
  `TimerChannel`, the observers, `BaseTracer` and `SpanSubmitter`, the actor storage hooks and
  `Worker::Api`. These are C++ interfaces with no Rust trait. Each is a seam interface that gets a
  trait and both adapters.

## Seam interfaces

A _seam interface_ has three parts:

- **A Rust trait**, in Rust types. For requests in, it is `worker::Interface`.
- **A `Rust*` adapter**: a C++ class over a Rust trait object, so that C++ calls Rust.
  `RustWorkerInterface` (`src/rust/worker/bridge.h`) is a C++ `WorkerInterface` that forwards each
  event to a Rust `worker::Interface`.
- **A `Cxx*` adapter**: a Rust type over a C++ object, so that Rust calls C++. `CxxWorkerInterface`
  (`src/rust/worker/cxx_worker.rs`) is a Rust `worker::Interface` that forwards each event to a C++
  `WorkerInterface`.

With both adapters, either side can hold an implementation from the other without knowing its
language.

## Ground rules

1. **Every seam interface is a Rust trait plus a `Rust*` and a `Cxx*` adapter.** Callers on either
   side don't know which language implements it.
2. **The C++ interface is a contract with embedders.** Changing or removing a virtual method needs a
   matching change in every embedder. A new method needs a default that keeps existing
   implementations compiling.
3. **Behaviour changes go behind an autogate.** Until the gate is removed, a Worker sees the same
   behaviour on both paths. A difference that no Worker can observe, such as one that only a
   runtime bug reaches, is documented where the Rust path makes it. The `@all-autogates` variant
   of each test takes the Rust path.
4. **Keep the shape.** See "Porting C++ to Rust" in
   [`src/workerd/io/AGENTS.md`](../../src/workerd/io/AGENTS.md#porting-c-to-rust).
5. **Runtime code doesn't depend on its executor.** No tokio in runtime crates. I/O and spawning go
   through the layer below, so that the code runs on the KJ event loop.
6. **The C++ side runs when a step is called**, not when its future is first polled. A step that
   waits returns a future for the wait only. The scheduled event splits into a synchronous start and
   an async finish for this reason (`src/workerd/io/entrypoint/`).
7. **No C++ JSG in the C++ shims behind Rust bindings.** The shims are listed in `CPP_SHIMS`
   (`src/rust/jsg/BUILD.bazel`), and `//src/rust/jsg:cpp-jsg-allowlist-test` fails on any use that
   isn't allowlisted.
8. **An interface on a hot path gets a benchmark before its Rust path ships.**

## Naming

"Rust seam" is the boundary described here. "Seam interface" is one trait with its two adapters. The
TypeScript and C++ `EventTarget` bootstrap boundary (`api/event-target-bootstrap.h`) is also called
a seam; it is unrelated.
