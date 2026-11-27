// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The I/O the worker factories on one Rust-owned event loop run on. The command line builds it on
// its loop (bootstrap.h) and each factory built over it (per-process.h) holds a reference.
//
// Do not include a generated bridge header here: the server crate's bridge includes this header.

#include <kj/memory.h>
#include <kj/refcount.h>

namespace kj {
class EntropySource;
class Filesystem;
class MonotonicClock;
class Network;
class Timer;
}  // namespace kj

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
}  // namespace kj_rs_tokio

namespace workerd::server {

// The I/O a factory runs on. Everything here must outlive the factory and every I/O object it
// made.
struct FactoryIo {
  kj::Timer& timer;
  // Consistent with `timer`.
  const kj::MonotonicClock& monotonicClock;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Filesystem& fs;
};

// The I/O of the factories on one loop: the loop's timer and monotonic clock, a tokio-backed
// kj::Network over the loop, the disk and the process's entropy. None of it keeps state that one
// factory could observe of another, so the factories on a loop share it. Lives on the loop's
// thread, and must not outlive the loop (the timer and the network are the loop's).
class LoopIo final: public kj::Refcounted {
 public:
  // Over `loop`, which must be the current thread's.
  static kj::Rc<LoopIo> start(kj_rs_tokio::TokioAsyncIoContext& loop);

  class Impl;
  explicit LoopIo(kj::Own<Impl> impl);
  ~LoopIo() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(LoopIo);

  const FactoryIo& get() const {
    return io;
  }

 private:
  kj::Own<Impl> impl;
  // Into `impl` and the loop.
  FactoryIo io;
};

}  // namespace workerd::server
