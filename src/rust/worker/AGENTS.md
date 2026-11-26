# src/rust/worker/

## OVERVIEW

The `worker` crate: the requests-in side of the [Rust seam](../../../docs/reference/rust-seam.md). `worker::Interface` (`lib.rs`) is the Rust counterpart of C++ `WorkerInterface` (`io/worker-interface.h`), with one method per event: `request()` and `connect()` from `kj::http::Service`, then `prewarm()`, `run_scheduled()`, `run_alarm()`, `abandon_alarm()`, `test()`, `bench()` and `custom_event()`. Two adapters carry it across the language boundary in each direction. The crate also has a few small implementations of `Interface`.

## KEY FILES

| File                      | Role                                                                                                                                                                          |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lib.rs`                  | The `Interface` trait, the result types, and `not_supported()`, which answers a `CustomEvent` as C++ `event->notSupported()` does                                              |
| `ffi.rs`                  | The cxx bridge. `Wrapper` boxes a `dyn Interface` for C++; the `worker_*` functions call a C++ `WorkerInterface` from Rust                                                    |
| `bridge.h`                | `RustWorkerInterface`, the `Rust*` adapter: a C++ `WorkerInterface` over a `Wrapper`. `fromImpl()` makes one from a `Box<Wrapper>`; `newPromisedInterface()` wraps a starting worker |
| `ffi.h/c++`               | The C++ side of the `worker_*` functions, and `RustWorkerInterface::bench()`                                                                                                  |
| `cxx_worker.rs`           | `CxxWorkerInterface`, the `Cxx*` adapter: a Rust `Interface` that forwards every event to an owned C++ `WorkerInterface`                                                     |
| `promised.rs`             | `PromisedInterface`: an `Interface` whose target is still starting, as C++ `PromisedWorkerInterface`                                                                           |
| `failure.rs`              | The worker that `WorkerInterface::fromException()` makes, behind `RUST_FAILURE_WORKER_INTERFACE`                                                                               |
| `error.rs`, `ok.rs`, `kill_switch.rs` | Workers that answer every event the same way: with an error, with OK, or as killed                                                                                                                                 |
| `test.c++`                | C++ tests of the implementations and of both adapters                                                                                                                                                    |

## CONVENTIONS

- **Crossing the boundary.** A Rust implementation becomes a C++ `WorkerInterface` with `Interface::into_kj()`, which goes through `RustWorkerInterface`. Rust code holds a C++ `WorkerInterface` as `CxxWorkerInterface::new(KjOwn<WorkerInterface>)`. The Rust server (`src/workerd/server/server/`) does both: its services implement `Interface`, and it reaches C++ Workers through `CxxWorkerInterface`.
- **Embedders keep the C++ interface.** A C++ embedder implements and calls C++ `WorkerInterface` as before; it never needs to see this crate.
- **Adding an event** touches each layer: a method on `Interface`, with a default where C++ `WorkerInterface` has one; a `Wrapper` method in `ffi.rs` and its override in `RustWorkerInterface`; a `worker_*` function in `ffi.rs` and `ffi.h/c++`, and its use in `CxxWorkerInterface`. Results cross as the shared structs in `ffi.rs`, kept in sync with `io/worker-interface.h`.
- **The `worker_*` functions take times as nanoseconds** since the Unix epoch (`kj_rs::repr::{to,from}Nanos`), not as `KjDate`.
