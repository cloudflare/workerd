# src/rust/cxx/

## Overview

This directory contains workerd's in-tree fork of cxx-rs. It was imported from the former
`cloudflare/workerd-cxx` repository. Changes to the fork and its workerd consumers should use the
in-tree Bazel labels and land atomically.

The fork adds KJ exceptions, smart pointers, data types, and bidirectional async interop. Do not
assume stock cxx-rs behavior when changing bridge generation or runtime code.

## Build and test

Run commands from the workerd repository root:

```sh
bazel build //src/rust/cxx/...
bazel test //src/rust/cxx/...
```

Important targets:

- `//src/rust/cxx:cxx` — Rust runtime crate
- `//src/rust/cxx:core` — C++ runtime and public header
- `//src/rust/cxx:codegen` — C++ bridge generator
- `//src/rust/cxx/kj-rs` — KJ integration crate and C++ support library

Dependencies come from workerd's `@crates_vendor` repository and `@capnp-cpp`; do not add a nested
Bazel module, Cargo workspace, toolchain configuration, or external `workerd-cxx` repository.

## Architecture

- `src/` and `include/` — cxx Rust and C++ runtimes
- `syntax/`, `gen/`, and `macro/` — bridge parser and code generators
- `kj-rs/` — KJ promises/futures, exceptions, ownership, refcounting, dates, and `Maybe`
- `kj-rs-tokio/` — `TokioEventPort`: a `kj::EventPort` backed by a per-thread tokio
  `current_thread` runtime, plus `setupTokioAsyncIo()` (no I/O providers) and
  `kj_rs_tokio::spawn()`
- `kj-hyper/` — HTTP/1.1, WebSockets and TLS for the Rust server: hyper's server and pooled
  client, rustls, the WebSocket handshake (kj's own `kj::WebSocket` runs over the upgraded
  transport), with kj-typed seams (`kj::http::Service`, `kj::HttpService::Response`,
  `kj::WebSocket`, `kj::AsyncIoStream`) so requests reach a C++ `WorkerInterface` and C++ can
  make outbound requests; see "kj-hyper" below
- `kj-rs-io/` — tokio-backed `kj::AsyncIoStream` / `kj::Network` / `kj::LowLevelAsyncIoProvider`
  (the I/O providers for the tokio loop, `kj_rs_io::setupTokioAsyncIo()`), `loopback:` addresses
  (in-process connections for `workerd test`), the `--watch` file watcher (Rust over `notify`),
  and signals. C++ there is interface adaptation only; the one policy
  object that stays C++ is `PeerFilter`, a wrapper over KJ's own `kj::_::NetworkFilter`, which
  Rust consults through a bridged `should_allow`
- `tests/` and `kj-rs/tests/` — Rust and C++ bridge integration tests
- `tools/bazel/` — Bazel bridge-generation macro used by this component's tests

## Conventions

- Follow the parent `src/rust/AGENTS.md` and repository `AGENTS.md`.
- Prefer KJ C++ types over STL types unless required by the cxx ABI.
- Preserve cancellation when converting between KJ promises and Rust futures.
- Every unsafe Rust block needs a `// Safety:` explanation.
- `KjOwn<T>` requires `T: kj_rs::OwnTarget`, generated per bridge for every declared type held in
  a `KjOwn` (see `kj-rs/README.md`). A type that is only aliased into a bridge gets no
  implementation there; add `impl KjOwn<T> {}` to the bridge that declares `T`.
- Run formatting and the full component tests after changing generated ABI behavior.

## kj-rs-io ownership and reactor rules

kj-rs-io/lib.rs is the canonical statement of these rules; this is the summary for people editing
the crate. Two kinds of guarantee are in play and must not be confused: what the **Rust handles**
guarantee by type, and what the **C++ adapters** guarantee by construction.

- **Rust handles: `Send + Sync` by type, operations own their state.** Shared state is `Arc`,
  atomics and `Mutex`; lib.rs asserts `Send + Sync` for every handle type at compile time, so an
  `Rc` or a `Cell` in one fails the build. Every bridged operation captures a share of its
  object's state, never a borrow (`fn -> impl Future + use<..>`), so a `rust::Box` destroyed or
  carried to another thread is never a memory-safety question.
- **Two port checks, at registration and at the wait.** A `TokioEventPort` thread is a tokio
  runtime thread for its whole life (kj-rs-tokio/port.rs), so tokio's constructors register with
  the loop's driver as they are; do not add steering (entering handles, wrapper constructors) or
  lints around them. `ensure_loop_thread()` goes before every registration; `ensure_owner_loop()`
  (the resource's recorded owner runtime) goes at the point an operation is about to wait for
  readiness, never on the fast path -- a read whose syscall completes at once pays nothing.
  Accept always waits, so it checks up front. The file watcher is runtime-independent (notify
  delivers from its own thread; `tokio::sync::Notify` needs no runtime) and checks nothing.
- **Raw handles and buffers become typed at the bridge.** `ffi.rs` is the crate's only module
  allowed to write `unsafe`. An fd/SOCKET arrives as an integer and becomes a `socket2::Socket` /
  `OwnedFd` in one `unsafe` block naming the C++ contract; a `tryRead` buffer arrives as pointer +
  length and becomes `&mut [MaybeUninit<u8>]`, never `&mut [u8]`. Those helpers are `unsafe fn`s:
  never make them safe, never pass integers or pointers past `ffi.rs`.
- **Addresses are typed on the bridge.** `SocketAddress` (ffi.rs) is the only form an address
  takes between Rust and C++; Rust converts it to and from `std`'s address types (safe), and
  async-io.c++ is the only place a `struct sockaddr` is decoded or encoded -- at the KJ interfaces
  that speak them. Do not reintroduce sockaddr byte blobs on either side.
- **C++ adapters: KJ's contracts, KJ's structure.** The adapters implement KJ interfaces and keep
  KJ's own shape where KJ has one: the connect and accept loops with `restrictPeers` filtering
  (KJ's own `kj::_::NetworkFilter` behind a `kj::Arc<PeerFilter>`, atomic because KJ allows an
  address to be cloned on another thread), fd-flag handling, accept-retry sets, socket options,
  backlog, and the operation-start policy (every returned I/O promise is started inside the call).
  The one KJ behavior deliberately *not* mirrored is when the syscall happens: reads and writes
  are tokio's `try_*` + readiness loops (stream.rs, "When the syscall happens"); do not add
  direct-syscall-first paths. Buffer validity until a promise settles and fd ownership under
  `TAKE_OWNERSHIP` remain KJ's documented contracts, named at the `// Safety:` comments.
- **kj-rs-io is workerd's provider, not a general KJ provider.** lib.rs, "Scope: workerd's
  provider", lists what is left out and the rule: no consumer in workerd's production code *or
  its configuration surface* (workerd.capnp's documented address grammar counts -- check it
  before declaring a feature unused), and hand-written libc/sockaddr/fd code to keep.
- **`--config=asan` and the `tsan` configs instrument both C++ and Rust.** Rust is built with
  nightly rustc, `-Zsanitizer=<address|thread>`, and a standard library instrumented the same way
  (//build/rust); `//src/rust/asan` and `//src/rust/tsan` verify the instrumentation is active.

## kj-hyper: the Rust-facing API and its rules

kj-hyper is used from Rust; its C++ (kj-hyper.c++) exists only to implement kj interfaces over
Rust objects, and all of its Rust `unsafe` is in ffi.rs.

- **Surface.** `server::serve_connection(io, &HeaderTable, Rc<ServerSettings>, Rc<dyn Handler>,
  &Shutdown)` serves one accepted tokio connection; `Handler::request` takes exactly what
  `kj::http::Service::request` takes, `Handler::connect` takes a `server::Connect` that is
  answered in Rust (`accept()` hands back the tunnel as tokio I/O, `reject()`) or handed to C++
  (`into_kj()`). `client::Client` implements `kj::http::Service` over hyper-util's legacy client:
  `Client::new(table, settings, dial)` pools connections to one peer through a dialer,
  `Client::internet(table, settings, tls, allow)` dials whatever authority a request's URL names.
  `tls` builds rustls configs from `TlsOptions` and runs the handshakes. `into_kj_stream` /
  `tcp_into_kj_stream` hand tokio streams to C++ as `kj::AsyncIoStream`.
- **Threads.** Everything is single-threaded state (`Rc`, `RefCell`, `Cell`), bound to the tokio
  runtime thread it was created on, as kj's own HTTP objects are bound to their event loop's
  thread. Transports must be `Send` (hyper's upgrade path and the legacy client's connection
  tasks require it); handlers, settings and header tables need not be. hyper's timers (header
  timeout, pool idle eviction) and the client's connection tasks (`tokio::spawn`) need the
  thread's tokio runtime entered.
- **Lifetimes.** A `HeaderTable` and `ServerSettings`/`ClientSettings` outlive the futures made
  with them (the server borrows the table; a `WebSocketErrorHandler` in the settings is borrowed
  by every response object built while a call runs). What a handler call borrows from C++ -- a
  `tryRead` buffer, header slices -- is tied to the future's lifetime on the bridge; Rust objects
  behind kj interfaces (`RustIo`, `RustBody`) are `Rc`s whose operations own a share, so a
  `kj::Own` dropped mid-operation dangles nothing.
- **Cancellation.** A handler call runs alongside its connection, not inside hyper's service
  future, and is dropped when the connection ends without an upgrade (kj's cancel-on-hangup). A
  failure after the response head went out aborts the body, so hyper drops the connection
  instead of framing a truncated message as complete. Dropping a client request drops its
  connection (hyper's rule); the request body pump ends with the exchange, as kj's adapter's
  does.
- **What is not observed.** A transport handed to C++ with `into_kj_stream` has no
  `whenWriteDisconnected()` (it never resolves; reads and writes fail once the peer is gone), and
  so a `kj::WebSocket` over a hyper-upgraded stream never resolves `whenAborted()` either (kj
  derives it from that signal): peer hangups are seen by reading. `tcp_into_kj_stream` goes
  through kj-rs-io, which does observe them.
- **Headers are copied at the crossing.** kj headers reach hyper through `for_each_header` ->
  `Head::append` (spellings kept in hyper's `HeaderCaseMap`, made public by
  patches/rust/hyper-public-header-case-map.patch); hyper's come back packed (`HeaderBlock`) and
  C++ checks the block's bounds before building `kj::HttpHeaders`.
- **WebSockets are kj's.** kj-hyper does the handshake (`handshake.rs`: accept and client keys;
  the extension agreement is kj's own parser, applied in kj-hyper.c++) and hands the upgraded
  transport to `kj::newWebSocket`, with frame-mask entropy from `rand` (`RustEntropySource`).
  A stream read and written by two tasks (a `RustIo`) is wrapped in `SharedWakers`, since
  rustls' reads write and its writes read.
- **Tests.** Rust unit tests sit in each module; kj-hyper/tests/ holds the contract tests
  against kj's HTTP interfaces (a C++ `kj::HttpService` served by kj-hyper, kj-hyper's client
  behind `kj::newHttpClient(kj::HttpService&)`, checked on the wire), a `kj_test` driving Rust
  helpers on the tokio-backed `kj::setupAsyncIo()`. TLS policy tests live in tls.rs with
  fixed PEM material (no certificate generation at test time).
- **Dropped kj settings.** No `HttpServerErrorHandler` (kj's default policy is built in; a
  handler wanting other answers handles its own errors), no `HttpClientErrorHandler`, no
  caller-supplied `EntropySource` (handshake keys and frame masks come from `rand`), no
  `tlsStarter`, no pipeline timeout or cancelled-upload grace period, and no cipher list
  (`TlsOptions.cipherList` is not applied: cipher suites are rustls' defaults); hyper answers
  unparsable requests itself.
