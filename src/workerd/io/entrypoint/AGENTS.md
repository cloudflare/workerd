# io/entrypoint/

## OVERVIEW

The `entrypoint` crate: the events of a C++ `WorkerEntrypoint` (`io/worker-entrypoint.{h,c++}`) that Rust delivers. `WorkerEntrypoint` hands Rust an `entrypoint::Invocation` (`io/entrypoint-invocation.h`): the `IoContext::IncomingRequest` the event was made for, the `waitUntil` task set, and what finds the exported handler (entrypoint name, version info, props). Rust reports the event to the tracer, delivers it, starts the handler and finishes the request, through C++ calls in `ffi.h`.

Two events are delivered this way:

- **The test event** (`WorkerEntrypoint::test()`, `test.rs`), with no autogate, since only test runners deliver it. Its handler runs in Rust: `Delivered::run_in_wait_until()` runs a Rust callback in a waitUntil task, under the isolate lock, on the handler's handles, which the test-api crate calls with a Rust `TestController`.
- **The scheduled event** (`WorkerEntrypoint::runScheduled()`, `scheduled.rs`), behind the `RUST_ENTRYPOINT_SCHEDULED` autogate. Its JavaScript half, `ServiceWorkerGlobalScope::startScheduled()` with the `ScheduledController` it creates, stays in C++ and runs in the `IoContext::run()` lambda of `invocation_start_scheduled_in_wait_until()`.

## KEY FILES

| File           | Role                                                                                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lib.rs`       | `Invocation` → `Delivered` → `Running`: each step consumes the value it is called on, so each happens once and in order. Dropping a delivered event drains it |
| `scheduled.rs` | The scheduled event's steps, in the order `WorkerEntrypoint::runScheduled()` takes them in C++                                                        |
| `test.rs`      | The test event's steps, and the callback that calls the handler's `test()`                                                                            |
| `ffi.rs`       | The cxx bridge, and the Rust functions C++ calls (`scheduled_event_*`, `test_event_*`, `handler_callback_run`, `wait_until_continuation_wait`)                                                 |
| `ffi.h/c++`    | The C++ calls, and `runScheduled()` and `runTest()`, which `worker-entrypoint.c++` calls                                                              |

## CONVENTIONS

- **C++ is called when a step is called**, not when a future it returns is first polled. A step that waits returns a future for the wait only, so the request is in the state the step leaves it in once the step returns, as in C++. `runScheduled()` and `runTest()` keep this across the boundary by splitting into a synchronous start and an async finish, so that an exception before the wait is thrown from `runScheduled()`, as in C++.
- **No panic reaches C++.** kj-rs polls and drops Rust futures through `extern "C"` functions, and cxx aborts on a panic in an `extern "Rust"` function. Synchronous entry points catch panics and fail with a KJ exception (`catch_panic` in `ffi.rs`); futures handed to C++ are wrapped in `io_context::CatchPanics`.
- **No C++ JSG in `ffi.c++` / `ffi.h`.** They are in `CPP_SHIMS` (`src/rust/jsg/BUILD.bazel`), with no allowlist entries, so `//src/rust/jsg:cpp-jsg-allowlist-test` fails on any use. Code that needs C++ JSG belongs in `src/workerd/io` or `src/workerd/api`, called from the shim: `startScheduled()`, and `getHandlerHandles()` (`io/entrypoint-invocation.{h,c++}`), which returns the handler's handles as `v8::Local`s. A Rust callback's run ends with io-context's `endRunOfIoCallback()`, which holds the one reviewed `JsExceptionThrown` that tells `IoContext::run()` of an exception the callback left pending.
- **A callback fails with a `RunError`**: a JavaScript error, which `IoContext::run()` logs as uncaught, or a KJ exception, which it gets unchanged, as from C++ code. A missing handler fails with a KJ exception for a JavaScript `Error` (`jsg.Error: ...`), as `JSG_REQUIRE` does in C++, so that it is logged once, not also as thrown by JavaScript.
- **Perfetto spans are emitted from C++.** Rust has no perfetto bindings yet, so a step that should have a span gets it in its C++ call; Rust passes the span's name as a `&'static CStr` where the call is shared between events. Both events keep the span names they had in C++; `entrypoint::runScheduled()` marks the scheduled event's Rust path. `server/tests/scheduled:scheduled-perfetto-test` and `server/tests/test-event:test-event-perfetto-test` check them.
- **The scheduled event's C++ path stays until the autogate is removed.** Keep the two paths' behaviour identical; `api/tests:scheduled-test` (its `@all-autogates` variant takes the Rust path) and `server/tests/scheduled` check both.

## NOT YET PORTED

- `bench()`: `workerd bench`'s event, once `BenchController` is in Rust.
- `ScheduledController` and `startScheduled()` in Rust, which would make the scheduled event's JavaScript half Rust as well.
- A Rust `WorkerEntrypoint` that implements `worker::Interface`, handling the ported events and delegating the rest to the C++ object. The per-event bridge entry points (`scheduled_event_*`, `test_event_*`) then become methods of that interface, which `RustWorkerInterface` already bridges.
- Perfetto spans from Rust, once Rust has perfetto bindings.
