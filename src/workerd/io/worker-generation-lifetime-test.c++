// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-generation.h"

#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

class DisposalObserver final: public IsolateObserver {
 public:
  DisposalObserver(const WorkerGeneration& generation,
      kj::WaitScope& waitScope,
      bool& teardownFinished,
      bool& observerDestroyed)
      : generation(generation),
        waitScope(waitScope),
        finished(teardownFinished),
        destroyed(observerDestroyed) {}

  ~DisposalObserver() noexcept(false) override {
    KJ_EXPECT(finished);
    KJ_EXPECT(!generation.whenTerminated().poll(waitScope));
    destroyed = true;
  }

  void teardownFinished() override {
    KJ_EXPECT(!generation.whenTerminated().poll(waitScope));
    finished = true;
  }

 private:
  const WorkerGeneration& generation;
  kj::WaitScope& waitScope;
  bool& finished;
  bool& destroyed;
};

KJ_TEST("generation acknowledgement follows real worker isolate teardown") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation =
      kj::atomicRefcounted<WorkerGeneration>(kj::heapArray<WorkerGeneration::Memory>(0));
  generation->claim();
  generation->beginStartup();
  bool teardownFinished = false;
  bool observerDestroyed = false;
  auto joined = generation->whenTerminated();

  {
    TestFixture fixture(TestFixture::SetupParams{
      .waitScope = waitScope,
      .useRealTimers = false,
      .isolateObserver = kj::atomicRefcounted<DisposalObserver>(
          *generation, waitScope, teardownFinished, observerDestroyed),
    });
    fixture.enterWorkerLockSynchronously([&](Worker::Lock& lock) {
      // The fixture owns a mutable isolate; production attaches it before compiling the script.
      auto& isolate =
          const_cast<Worker::Isolate&>(Worker::Isolate::from(jsg::Lock::from(lock.getIsolate())));
      isolate.setGeneration(kj::atomicAddRef(*generation));
    });
    generation->finishStartup();
    generation->requestStop();
    KJ_EXPECT(!joined.poll(waitScope));
    KJ_EXPECT(!teardownFinished);
    KJ_EXPECT(!observerDestroyed);
  }

  KJ_EXPECT(teardownFinished);
  KJ_EXPECT(observerDestroyed);
  KJ_EXPECT(joined.poll(waitScope));
  joined.wait(waitScope);
  generation->whenTerminated().wait(waitScope);
}

}  // namespace
}  // namespace workerd
