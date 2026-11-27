// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "loop-io.h"

#include <workerd/util/entropy.h>

#include <kj-rs-io/async-io.h>
#include <kj-rs-tokio/tokio-event-port.h>

#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/time.h>

namespace workerd::server {
namespace {

class EntropySourceImpl final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    getEntropy(buffer);
  }
};

}  // namespace

// Members are declared in construction order, which is the reverse of destruction.
class LoopIo::Impl {
 public:
  explicit Impl(kj_rs_tokio::TokioAsyncIoContext& loop)
      : timer(loop.getTimer()),
        monotonicClock(kj::systemPreciseMonotonicClock()),
        provider(loop.getTimer()) {}
  KJ_DISALLOW_COPY_AND_MOVE(Impl);

  FactoryIo get() {
    return FactoryIo{
      .timer = timer,
      .monotonicClock = monotonicClock,
      .network = provider.getNetwork(),
      .entropySource = entropySource,
      .fs = *fs,
    };
  }

 private:
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  EntropySourceImpl entropySource;
  kj::Timer& timer;
  // The loop's monotonic clock, consistent with `timer`.
  const kj::MonotonicClock& monotonicClock;
  // The tokio-backed kj::Network over the loop.
  kj_rs_io::TokioAsyncIoProvider provider;
};

kj::Rc<LoopIo> LoopIo::start(kj_rs_tokio::TokioAsyncIoContext& loop) {
  return kj::rc<LoopIo>(kj::heap<Impl>(loop));
}

LoopIo::LoopIo(kj::Own<Impl> impl): impl(kj::mv(impl)), io(this->impl->get()) {}
LoopIo::~LoopIo() noexcept(false) {}

}  // namespace workerd::server
