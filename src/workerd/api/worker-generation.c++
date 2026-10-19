// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-generation.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/setup.h>

#include <cmath>

namespace workerd::api {

jsg::JsValue WorkerGenerationController::getSharedMemory(jsg::Lock& js, kj::String name) {
  for (auto& memory: generation->getMemories()) {
    if (memory.name == name) {
      return jsg::JsValue(v8::SharedArrayBuffer::New(js.v8Isolate, memory.backing));
    }
  }
  JSG_FAIL_REQUIRE(RangeError, "Unknown generation shared memory: ", name);
}

jsg::Promise<void> WorkerGenerationController::terminate(jsg::Lock& js) {
  (void)*owner;
  generation->requestStop();
  return whenTerminated(js);
}

void WorkerGenerationController::terminateAfter(jsg::Lock& js, double milliseconds) {
  (void)*owner;
  JSG_REQUIRE(!deadlineArmed, Error, "A generation deadline can only be armed once.");
  JSG_REQUIRE(std::isfinite(milliseconds) && milliseconds >= 0 && milliseconds <= 60000, RangeError,
      "Generation deadline must be between 0 and 60000 milliseconds.");
  generation->requireRunning();
  class StopTask final: public v8::Task {
   public:
    explicit StopTask(kj::Own<workerd::WorkerGeneration> generation)
        : generation(kj::mv(generation)) {}
    ~StopTask() noexcept override = default;

    void Run() override {
      generation->requestStop();
    }

   private:
    kj::Own<workerd::WorkerGeneration> generation;
  };
  jsg::IsolateBase::from(js.v8Isolate)
      .scheduleBackgroundTask(
          std::make_unique<StopTask>(kj::atomicAddRef(*generation)), milliseconds / 1000);
  deadlineArmed = true;
}

jsg::Promise<void> WorkerGenerationController::whenTerminated(jsg::Lock& js) {
  return IoContext::current().awaitIo(js, generation->whenTerminated());
}

jsg::Ref<WorkerGenerationController> WorkerGenerationController::create(
    jsg::Lock& js, jsg::Dict<uint32_t> sharedMemory) {
  JSG_REQUIRE(FeatureFlags::get(js).getEnableWorkerGenerations(), Error,
      "Worker generations require the enable_worker_generations compatibility flag.");
  JSG_REQUIRE(IoContext::current().getIoChannelFactory().supportsWorkerGenerations(), Error,
      "This runtime does not support colocated worker generations.");
  JSG_REQUIRE(sharedMemory.fields.size() <= 16, RangeError,
      "A generation supports at most 16 shared memories.");
  size_t total = 0;
  kj::Vector<workerd::WorkerGeneration::Memory> memories;
  for (auto& field: sharedMemory.fields) {
    total += field.value;
    JSG_REQUIRE(
        total <= 65536, RangeError, "Generation shared memory exceeds the 65536-byte limit.");
    JSG_REQUIRE(field.name.size() > 0, TypeError, "Shared memory names must not be empty.");
    memories.add(workerd::WorkerGeneration::Memory{
      .name = kj::mv(field.name),
      .backing = v8::SharedArrayBuffer::NewBackingStore(js.v8Isolate, field.value),
    });
  }
  auto generation = kj::atomicRefcounted<workerd::WorkerGeneration>(memories.releaseAsArray());
  auto owner = IoContext::current().addObject(
      kj::heap<WorkerGenerationController::Owner>(kj::atomicAddRef(*generation)));
  return js.alloc<WorkerGenerationController>(kj::mv(generation), kj::mv(owner));
}

}  // namespace workerd::api
