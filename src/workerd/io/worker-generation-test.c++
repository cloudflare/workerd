// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-generation.h"

#include <workerd/jsg/jsg-test.h>

#include <kj/test.h>
#include <kj/thread.h>

namespace workerd::jsg::test {
namespace {

struct GenerationContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(GenerationContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(GenerationIsolate, GenerationContext);

V8System v8System;

kj::Own<WorkerGeneration> newGeneration() {
  return kj::atomicRefcounted<WorkerGeneration>(kj::heapArray<WorkerGeneration::Memory>(0));
}

KJ_TEST("worker generation stop before claim is permanent and acknowledged") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();

  generation->requestStop();
  generation->requestStop();
  generation->whenTerminated().wait(waitScope);
  KJ_EXPECT_THROW_MESSAGE("jsg.Error: Worker generation stopped.", generation->claim());
  KJ_EXPECT(generation->isStopRequested());
}

KJ_TEST("worker generation rejects duplicate startup entry without releasing its reservation") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();
  generation->claim();
  generation->beginStartup();

  KJ_EXPECT_THROW_MESSAGE("startup is not pending", generation->beginStartup());
  auto termination = generation->whenTerminated();
  generation->requestStop();
  KJ_EXPECT(!termination.poll(waitScope));

  generation->finishStartup();
  termination.wait(waitScope);
}

KJ_TEST("worker generation startup entry after stop still releases its reservation") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();
  generation->claim();
  generation->requestStop();

  generation->beginStartup();
  auto termination = generation->whenTerminated();
  KJ_EXPECT(!termination.poll(waitScope));
  generation->finishStartup();
  termination.wait(waitScope);
}

KJ_TEST("worker generation termination waits for startup and joins live waiters") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();
  generation->claim();

  auto cancelledWait = generation->whenTerminated();
  cancelledWait = nullptr;
  auto firstWait = generation->whenTerminated();
  auto secondWait = generation->whenTerminated();
  auto stopWait = generation->onStop();
  auto cancelledStopWait = generation->onStop();
  cancelledStopWait = nullptr;

  generation->requestStop();
  KJ_EXPECT(!firstWait.poll(waitScope));
  KJ_EXPECT_THROW_MESSAGE("jsg.Error: Worker generation stopped.", stopWait.wait(waitScope));
  KJ_EXPECT_THROW_MESSAGE("jsg.Error: Worker generation stopped.", generation->requireRunning());
  KJ_EXPECT_THROW_MESSAGE(
      "jsg.Error: Worker generation stopped.", generation->onStop().wait(waitScope));

  generation->finishStartup();
  firstWait.wait(waitScope);
  secondWait.wait(waitScope);
  generation->whenTerminated().wait(waitScope);
  KJ_EXPECT_THROW_MESSAGE("jsg.Error: Worker generation stopped.", generation->claim());
}

KJ_TEST("worker generation acknowledges attached isolate only after disposal") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();
  auto lateGeneration = newGeneration();
  auto termination = generation->whenTerminated();

  {
    GenerationIsolate isolate(v8System, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](GenerationIsolate::Lock& lock) {
      generation->claim();
      generation->attachIsolate(lock.v8Isolate);
      lateGeneration->claim();
      lateGeneration->requestStop();
      KJ_EXPECT_THROW_MESSAGE(
          "jsg.Error: Worker generation stopped.", lateGeneration->attachIsolate(lock.v8Isolate));
    });

    generation->finishStartup();
    generation->requestStop();
    KJ_EXPECT(!termination.poll(waitScope));
    generation->detachIsolate();
    KJ_EXPECT(!termination.poll(waitScope));

    lateGeneration->finishStartup();
    lateGeneration->whenTerminated().wait(waitScope);
  }

  KJ_EXPECT(!termination.poll(waitScope));
  generation->isolateDisposed();
  termination.wait(waitScope);
}

KJ_TEST("worker generation serializes stop with isolate registration") {
  kj::EventLoop eventLoop;
  kj::WaitScope waitScope(eventLoop);
  auto generation = newGeneration();
  generation->claim();
  bool attached = false;
  bool rejectedBecauseStopped = false;

  {
    GenerationIsolate isolate(v8System, kj::heap<IsolateObserver>());
    v8::Isolate* rawIsolate = nullptr;
    isolate.runInLockScope([&](GenerationIsolate::Lock& lock) { rawIsolate = lock.v8Isolate; });

    kj::Thread stopper([&] { generation->requestStop(); });
    try {
      generation->attachIsolate(rawIsolate);
      attached = true;
    } catch (kj::Exception& exception) {
      rejectedBecauseStopped =
          exception.getDescription() == "jsg.Error: Worker generation stopped."_kj;
    }
    KJ_EXPECT(attached != rejectedBecauseStopped);
    generation->finishStartup();
    auto termination = generation->whenTerminated();
    if (attached) {
      KJ_EXPECT(!termination.poll(waitScope));
      generation->detachIsolate();
      KJ_EXPECT(!termination.poll(waitScope));
    }
  }

  auto termination = generation->whenTerminated();
  if (attached) {
    KJ_EXPECT(!termination.poll(waitScope));
    generation->isolateDisposed();
  }
  termination.wait(waitScope);
}

}  // namespace
}  // namespace workerd::jsg::test
