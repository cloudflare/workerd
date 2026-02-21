# src/workerd/io/

## OVERVIEW

I/O lifecycle, per-request context, worker/isolate management, actor storage, consistency gates, and compatibility flags.

## KEY CLASSES

| Class                                       | File                   | Role                                                                                        |
| ------------------------------------------- | ---------------------- | ------------------------------------------------------------------------------------------- |
| `IoContext`                                 | `io-context.{h,c++}`   | Per-request god object; thread-local via `IoContext::current()` (291+ call sites in `api/`) |
| `IoContext::IncomingRequest`                | `io-context.h`         | Tracks one inbound request for metrics/tracing; actors have many per IoContext              |
| `Worker`                                    | `worker.{h,c++}`       | Ref-counted worker instance; owns Script + Isolate refs                                     |
| `Worker::Isolate`                           | `worker.h:337`         | V8 isolate wrapper; shared across workers with same config                                  |
| `Worker::Script`                            | `worker.h:250`         | Compiled script bound to an Isolate                                                         |
| `Worker::Lock`                              | `worker.h:677`         | Synchronous V8 isolate lock (must hold to touch JS heap)                                    |
| `Worker::AsyncLock`                         | `worker.h:799`         | Fair async queue for acquiring `Worker::Lock`                                               |
| `Worker::Actor`                             | `worker.h:819`         | Durable Object instance; owns gates + cache + hibernation state                             |
| `ActorCache`                                | `actor-cache.{h,c++}`  | LRU write-back cache over RPC storage; `ActorCacheOps` base                                 |
| `ActorSqlite`                               | `actor-sqlite.{h,c++}` | SQLite-backed `ActorCacheOps` implementation                                                |
| `InputGate` / `OutputGate`                  | `io-gate.{h,c++}`      | Consistency primitives for DO concurrent request handling                                   |
| `IoOwn<T>` / `IoPtr<T>` / `ReverseIoOwn<T>` | `io-own.{h,c++}`       | Cross-heap smart pointers preventing KJ↔JS ref leaks                                        |

## WHERE TO LOOK

| Task                            | File(s)                                                                           |
| ------------------------------- | --------------------------------------------------------------------------------- |
| Add/modify compat flag          | `compatibility-date.capnp` (annotations define name + enable date)                |
| Promise bridging KJ↔JS          | `io-context.h` — `awaitIo()`, `awaitJs()`                                         |
| Request lifecycle / subrequests | `io-context.{h,c++}`, `worker-entrypoint.{h,c++}`                                 |
| Actor storage ops               | `actor-cache.h` (`ActorCacheOps`), `actor-sqlite.h`, `actor-storage.capnp`        |
| DO gate semantics               | `io-gate.{h,c++}` — `InputGate::CriticalSection`, `OutputGate::lockWhile()`       |
| Worker/isolate creation         | `worker.{h,c++}`, `worker-modules.{h,c++}`                                        |
| Metrics/logging hooks           | `observer.h` — `RequestObserver`, `IsolateObserver`, `ActorObserver`              |
| Tracing                         | `trace.{h,c++,capnp}`, `trace-stream.{h,c++}`, `tracer.{h,c++}`                   |
| Resource limits                 | `limit-enforcer.h` (abstract interface)                                           |
| Timer scheduling                | `io-timers.{h,c++}`                                                               |
| Hibernatable WebSockets         | `hibernation-manager.{h,c++}`                                                     |
| Cap'n Proto schemas             | `worker-interface.capnp`, `actor-storage.capnp`, `container.capnp`, `trace.capnp` |

## CONVENTIONS

- `IoContext::current()` — ambient thread-local access; only valid inside a request
- `awaitIo(js, kjPromise, func)` bridges KJ→JS; `func` runs under V8 lock. `awaitIo(js, kjPromise)` for identity
- `awaitJs(js, jsPromise)` bridges JS→KJ
- `addObject(kj::Own<T>)` returns `IoOwn<T>` — the **only** safe way to store KJ I/O objects reachable from JS heap
- Two-phase locking: `Worker::AsyncLock` (fair queue) → `Worker::Lock` (V8 isolate lock)
- `ActorCacheOps` methods return `kj::OneOf<Result, kj::Promise<Result>>` — sync when cached, async otherwise
- `OutputGate::lockWhile(promise)` blocks outgoing responses until the promise resolves
- `InputGate::CriticalSection` must succeed or permanently breaks the gate
- Observer classes (`RequestObserver`, `IsolateObserver`, etc.) have no-op defaults; all methods optional

## ANTI-PATTERNS

- **NEVER** hold JS heap refs to KJ I/O objects without `IoOwn`; enforced by `DISALLOW_KJ_IO_DESTRUCTORS_SCOPE`
- **NEVER** evaluate modules inside an IoContext; async I/O is **forbidden** in global scope
- **NEVER** use `getWaitUntilTasks()` — use `addWaitUntil()`
- **NEVER** use `awaitIoLegacy()` in new code — use `awaitIo()` with continuation
- `awaitIoImpl` parameter ordering (promise by-value, func by-ref) is **critical** for exception safety
- `abortWhen()` promises must **never** enter the V8 isolate
- Cross-request I/O object access throws by design (IoOwn prevents this)
