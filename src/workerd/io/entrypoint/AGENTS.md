# io/entrypoint/

## OVERVIEW

The `entrypoint` crate: a Rust `WorkerEntrypoint`, which receives one event for a Worker's exported handler. It owns the C++ `WorkerEntrypoint` (`io/worker-entrypoint.{h,c++}`), seen as an `entrypoint::CxxEntrypoint`, which makes the event's request. Behind the `RUST_WORKER_ENTRYPOINT` autogate, `WorkerEntrypoint::construct()` returns it in place of the C++ one.

C++ callers reach it through `RustWorkerEntrypoint` in `ffi.c++`, a `WorkerInterface` that sends an event Rust implements to Rust and every other event straight to the C++ object, with no Rust in between, so that `request()` and `connect()` cost no more than before. For an event that Rust implements, the C++ object hands Rust an `entrypoint::Invocation` (`io/entrypoint-invocation.h`): the `IoContext::IncomingRequest` the event was made for, the `waitUntil` task set, and what finds the exported handler (entrypoint name, version info, props). Rust reports the event to the tracer, delivers it, starts the handler and finishes the request, through C++ calls in `ffi.h`.

The scheduled event is the one event delivered this way. Its JavaScript half, `ServiceWorkerGlobalScope::startScheduled()` with the `ScheduledController` it creates, stays in C++: both paths start it with `entrypoint::startScheduledInWaitUntil()` (`io/entrypoint-invocation.{h,c++}`).

## KEY FILES

| File           | Role                                                                                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lib.rs`       | `WorkerEntrypoint`, which owns the C++ one. `Invocation` → `Delivered` → `Running`: each step consumes the value it is called on, so each happens once and in order. Dropping a delivered event drains it, even after a failed step |
| `scheduled.rs` | The scheduled event's steps, in the order `WorkerEntrypoint::runScheduled()` takes them in C++                                                        |
| `ffi.rs`       | The cxx bridge, and the Rust functions C++ calls (`worker_entrypoint_new`, `scheduled_event_start` / `scheduled_event_finish`) |
| `ffi.h/c++`    | The C++ calls, `RustWorkerEntrypoint`, and `newWorkerEntrypoint()`, which `WorkerEntrypoint::construct()` calls |

## CONVENTIONS

- **C++ is called when a step is called**, not when a future it returns is first polled. A step that waits returns a future for the wait only, so the request is in the state the step leaves it in once the step returns, as in C++. `RustWorkerEntrypoint::runScheduled()` keeps this across the boundary by splitting into a synchronous `scheduled_event_start` and an async `scheduled_event_finish`, so that an exception before the wait is thrown from `runScheduled()`, as in C++.
- **No panic reaches kj-rs.** The cxx bridge turns a panic in an `extern "Rust"` function into a KJ exception (`src/rust/cxx/README.md`), but kj-rs polls and drops Rust futures through `extern "C"` functions, where a panic aborts the process. Futures handed to C++ are wrapped in `io_context::CatchPanics`.
- **No C++ JSG in `ffi.c++` / `ffi.h`.** They are in `CPP_SHIMS` (`src/rust/jsg/BUILD.bazel`), with no allowlist entries, so `//src/rust/jsg:cpp-jsg-allowlist-test` fails on any use. Code that needs C++ JSG belongs in `src/workerd/io` or `src/workerd/api`, in a function that the C++ path calls too, so that the two paths can't drift apart (as `startScheduledInWaitUntil()` is).
- **Perfetto spans are emitted from C++.** Rust has no perfetto bindings yet, so a step that should have a span gets it in its C++ call. The scheduled event keeps the span names of the C++ path; `entrypoint::runScheduled()` marks the Rust path.
- **The C++ path stays until the autogate is removed.** Keep the two paths' behaviour identical. Every test's `@all-autogates` variant goes through the Rust `WorkerEntrypoint`; `api/tests:scheduled-test` and `server/tests/scheduled` check the scheduled event on both paths.
- **Performance:** every event of every Worker goes through `RustWorkerEntrypoint` when the gate is on. Measure `//src/workerd/tests:bench-global-scope` with the gate off and on before changing what an unported event costs.

## NOT YET PORTED

- `test()`: delivered through the crate with a generic Rust callback run under the isolate lock (`Delivered::run` / `run_in_wait_until`, with the handler's handles from `io`), which only `test()` needs.
- `bench()`: `workerd bench`'s event, once `BenchController` is in Rust.
- `ScheduledController` and `startScheduled()` in Rust, which would make the scheduled event's JavaScript half Rust as well.
- `request()` and `connect()`, which keep the C++ object's state (the request's `cf` blob, client address, whether to tunnel exceptions, the response-sent trackers, fail-open proxying).
- `worker::Interface` for the Rust `WorkerEntrypoint`, once `request()` is in Rust. `RustWorkerInterface` then replaces `RustWorkerEntrypoint`, and the per-event bridge entry points become the interface's methods.
- Perfetto spans from Rust, once Rust has perfetto bindings.
