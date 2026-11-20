# io/entrypoint/

## OVERVIEW

The `entrypoint` crate: the events of a C++ `WorkerEntrypoint` (`io/worker-entrypoint.{h,c++}`) that Rust delivers. `WorkerEntrypoint` hands Rust an `entrypoint::Invocation` (`io/entrypoint-invocation.h`): the `IoContext::IncomingRequest` the event was made for and what finds the exported handler (entrypoint name, version info, props). Rust reports the event to the tracer, delivers it, starts the handler and finishes the request. The calls on the request are `io_context`'s bindings of `IoContext::IncomingRequest` (`io-context/ffi.h`); this crate's `ffi.h` only reaches into its own `Invocation` and starts the handler (see "Porting C++ to Rust" in `io/AGENTS.md`).

The scheduled event is the one event delivered this way, behind its own autogate, `RUST_ENTRYPOINT_SCHEDULED` (`WorkerEntrypoint::runScheduled()`). Its JavaScript half, `ServiceWorkerGlobalScope::startScheduled()` with the `ScheduledController` it creates, stays in C++: both paths start it with `entrypoint::startScheduledInWaitUntil()` (`io/entrypoint-invocation.{h,c++}`).

## KEY FILES

| File           | Role                                                                                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lib.rs`       | `Invocation` → `Delivered` → `Running`: each step consumes the value it is called on, so each happens once and in order. Dropping a delivered event destroys its request without draining it, as C++ does after a failed step |
| `scheduled.rs` | The scheduled event's steps, in the order `WorkerEntrypoint::runScheduled()` takes them in C++                                                        |
| `ffi.rs`       | The cxx bridge, and the Rust functions C++ calls (`scheduled_event_start` / `scheduled_event_finish`)                                                 |
| `ffi.h/c++`    | Access to the `Invocation`'s request, the handler's start, and `runScheduled()`, which `worker-entrypoint.c++` calls                                                                              |

## CONVENTIONS

- **C++ is called when a step is called**, not when a future it returns is first polled. A step that waits returns a future for the wait only, so the request is in the state the step leaves it in once the step returns, as in C++. `runScheduled()` keeps this across the boundary by splitting into a synchronous `scheduled_event_start` and an async `scheduled_event_finish`, so that an exception before the wait is thrown from `runScheduled()`, as in C++.
- **No C++ JSG in `ffi.c++` / `ffi.h`.** They are in `CPP_SHIMS` (`src/rust/jsg/BUILD.bazel`), with no allowlist entries, so `//src/rust/jsg:cpp-jsg-allowlist-test` fails on any use. Code that needs C++ JSG belongs in `src/workerd/io` or `src/workerd/api`, in a function that the C++ path calls too, so that the two paths can't drift apart (as `startScheduledInWaitUntil()` is).
- **Perfetto spans keep the C++ path's names.** A step in Rust emits its span with `perfetto::trace_event!` (`util/perfetto`), never held across an `.await`. A span around C++ work that both paths share stays in that C++ code, as `WorkerEntrypoint::runScheduled() run` does in `startScheduledInWaitUntil()`. `entrypoint::runScheduled()` marks the Rust path.
- **One autogate per event.** Each event that production requests reach is moved behind an autogate of its own, which no later change renames or widens. An event that only workerd's own commands deliver (`test()`, `bench()`) needs none.
- **The C++ path stays until the autogate is removed.** Until then, a Worker sees the same behaviour on both paths. A difference that no Worker can observe, such as one that only a runtime bug reaches, is documented where the Rust path makes it, as `Running::finish_scheduled()` does for `finishScheduled()`'s exceptions. `api/tests:scheduled-test` (its `@all-autogates` variant takes the Rust path) and `server/tests/scheduled` check both.

## NOT YET PORTED

- `test()`: delivered through the crate with a generic Rust callback run under the isolate lock (`Delivered::run` / `run_in_wait_until`, with the handler's handles from `io`), which only `test()` needs.
- `bench()`: `workerd bench`'s event, once `BenchController` is in Rust.
- `ScheduledController` and `startScheduled()` in Rust, which would make the scheduled event's JavaScript half Rust as well.
- A Rust `WorkerEntrypoint` that implements `worker::Interface`, handling the ported events and delegating the rest to the C++ object. The per-event bridge entry points (`scheduled_event_start` / `scheduled_event_finish`) then become methods of that interface, which `RustWorkerInterface` already bridges.
