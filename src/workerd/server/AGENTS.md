# server/

## OVERVIEW

Binary entry point + orchestration layer. `CliMain` (workerd.c++) dispatches subcommands (`serve`, `compile`, `test`, `fuzzilli`, `pyodide-lock`) via `kj::MainBuilder`. `Server` (server.c++, ~6K lines) is the god object: parses `workerd.capnp` config, constructs all service types as nested inner classes, wires sockets/bindings/actors, runs the event loop.

## KEY FILES

| File                     | Role                                                                                                                                                                                          |
| ------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `workerd.c++`            | `CliMain`: subcommand dispatch, config loading, file watching, signal handling                                                                                                                |
| `server.h/c++`           | `Server` class: 15+ nested inner classes (`WorkerService`, `NetworkService`, `ExternalHttpService`, `DiskDirectoryService`, etc.). Two-phase init: `startServices()` then `listenOnSockets()` |
| `workerd.capnp`          | Config schema: `Config`, `Service`, `Worker`, `Socket`, `Extension`. Capability-based security model                                                                                          |
| `workerd-api.h/c++`      | `WorkerdApi`: registers all JS API types with JSG, compiles modules/globals, extracts source from config. `Global` struct has 20+ binding variants as `kj::OneOf`                             |
| `alarm-scheduler.h/c++`  | DO alarm scheduling with SQLite-backed persistence                                                                                                                                            |
| `json-logger.h/c++`      | Structured JSON logging for tail workers                                                                                                                                                      |
| `channel-token.h/c++`    | Opaque token encoding for cross-service channel references                                                                                                                                    |
| `v8-platform-impl.h/c++` | Custom `v8::Platform` bridging V8 tasks to KJ event loop                                                                                                                                      |
| `fallback-service.h/c++` | Module fallback resolution via external service                                                                                                                                               |
| `container-client.h/c++` | Experimental (2025): Docker container lifecycle for DO containers                                                                                                                             |
| `docker-api.capnp`       | Cap'n Proto schema for container management                                                                                                                                                   |
| `pyodide.h/c++`          | Python worker preloading and snapshot management                                                                                                                                              |

## TEST INFRASTRUCTURE

`server-test.c++` (~6K lines): integration tests using inline Cap'n Proto config strings. Tests full server lifecycle with real V8 isolates.

`tests/server-harness.mjs`: Node.js harness spawning `workerd` child processes for E2E tests. Subdirectories: `compile-tests`, `container-client`, `extensions`, `inspector`, `python`, `structured-logging`, `unsafe-eval`, `unsafe-module`, `weakref`.

Pattern: unit tests (`*-test.c++`) at directory level; integration/E2E tests in `tests/` using the harness.

## I/O BACKEND SEAM (--//:io_backend)

- The process event loop and every socket are tokio-backed by default (kj-rs-tokio + kj-rs-io).
  `--//:io_backend=cxx` keeps the all-C++ stack selectable; CI only builds that arm.
- Only two libraries may `#if WORKERD_RUST_IO_BACKEND_RUST`: `//src/workerd/util:setup-async-io`
  (supplies `kj::setupAsyncIo()`) and `:cli-io-backend` (the --watch watcher and SIGTERM). Call
  sites stay backend-agnostic; do not add the define to other targets.
- Never depend on the `@capnp-cpp//src/kj:kj-async` umbrella: it drags in `kj-async-os`, whose
  definitions collide with the shim's (an ODR violation). Use `:kj-async-core` / `:kj-async-io`.
  `//src/workerd/server:rust-io-hermeticity` fails analysis on any such edge and is part of every
  wildcard build (not `manual`); `:rust-io-link-check` inspects the linked binary on unix.
- Ownership under rust: the returned `kj::AsyncIoContext` borrows a heap holder attached to its
  `lowLevelProvider`; the holder owns the tokio context and the inert event port, so the context's
  references are valid for its whole lifetime and torn down once. The inert `kj::UnixEventPort` is
  never driven (`KJ_UNIMPLEMENTED` if anything tries). I/O objects are used on the loop thread that
  created them (kj-rs-io checks and throws otherwise).
