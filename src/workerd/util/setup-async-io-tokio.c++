// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Provides the kj::setupAsyncIo() *symbol* for the rust I/O backend, so every existing
// kj::setupAsyncIo() call site links unchanged under --//:io_backend=rust -- no per-call-site
// #if, no divergence from upstream kj (kj/async-io.h is untouched).
//
// Under --//:io_backend=rust the native OS event loop (kj-async-os, which defines both
// kj::setupAsyncIo() and kj::UnixEventPort's member functions) is NOT linked. This TU supplies a
// tokio-backed kj::setupAsyncIo() that repackages src/rust/cxx/kj-rs-io's setupTokioAsyncIo() into a
// kj::AsyncIoContext.
//
// kj::AsyncIoContext still declares `UnixEventPort& unixEventPort` (a concrete reference we must
// not change -- it's upstream kj public API). kj::UnixEventPort is `final`, but that only forbids
// subclassing; its method *definitions* live in async-unix.c++, which is absent from this link.
// So we define an inert kj::UnixEventPort here (its ctor/dtor + the EventPort/SleepHooks virtuals)
// with no ODR competitor, construct one, and bind the reference to it. It is never driven -- the
// event loop runs on kj_rs_tokio::TokioEventPort; signals use kj_rs_io::onSignal() -- and nothing
// reads AsyncIoContext::unixEventPort under the rust backend (verified: the only readers, SIGTERM
// drain + --watch, are #if !WORKERD_RUST_IO_BACKEND_RUST). Its methods KJ_UNIMPLEMENTED as a
// backstop: if anything ever does drive it, the link/run fails loudly rather than silently.
//
// The whole TU is gated on WORKERD_RUST_IO_BACKEND_RUST so that in the default (cxx) build it is
// empty and cannot ODR-clash with kj-async-os's real definitions.
//
// In the rust config the reverse hazard exists: if kj-async-os is ever accidentally linked back
// in, this TU's definitions collide with the native ones, and with static archives the winner is
// LINK-ORDER-DEPENDENT -- a duplicate-symbol error if you are lucky, the native setupAsyncIo
// silently winning (wrong event loop) or pairing with this inert UnixEventPort
// (KJ_UNIMPLEMENTED at runtime) if you are not. The guard against that is the build-graph gate
// //src/workerd/server:rust-io-hermeticity (build/rust_io_backend.bzl), which is tagged `manual`:
// it is NOT implied by building :workerd and must be built explicitly in the rust-config CI lane.

#if WORKERD_RUST_IO_BACKEND_RUST

#include <kj-rs-io/async-io.h>

#include <kj/async-io.h>
#if _WIN32
#include <kj/async-win32.h>
#else
#include <kj/async-unix.h>
#endif
#include <kj/time.h>

namespace kj {

#if _WIN32

// ---- Inert Win32EventPort ----------------------------------------------------------------------
// On Windows kj::AsyncIoContext declares `Win32EventPort& win32EventPort`. Unlike UnixEventPort,
// Win32EventPort is an abstract interface, so instead of supplying out-of-line definitions for an
// unlinked concrete class we define our own inert implementation. Same contract as the unix inert
// port below: never driven, KJ_UNIMPLEMENTED as a backstop.
class InertWin32EventPort final: public Win32EventPort {
 public:
  InertWin32EventPort(): clock(systemPreciseMonotonicClock()), timerImpl(clock.now()) {}

  bool wait() override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  bool poll() override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  void wake() const override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  Own<IoObserver> observeIo(HANDLE handle) override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  Own<SignalObserver> observeSignalState(HANDLE handle) override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  void allowApc() override {
    KJ_UNIMPLEMENTED("Win32EventPort is inert under --//:io_backend=rust (tokio drives the loop)");
  }
  Timer& getTimer() override {
    return timerImpl;
  }

 private:
  const MonotonicClock& clock;
  TimerImpl timerImpl;
};

using InertEventPort = InertWin32EventPort;

#else  // _WIN32

// ---- Inert kj::UnixEventPort (real definitions live in the unlinked async-unix.c++) -----------

#if !KJ_USE_KQUEUE
// On epoll platforms UnixEventPort holds a `Maybe<Own<ChildSet>>` whose type is only
// forward-declared in async-unix.h; the real definition lives in the unlinked async-unix.c++.
// Defining ~UnixEventPort() below requires the type to be complete. The inert port never

}  // namespace kj
