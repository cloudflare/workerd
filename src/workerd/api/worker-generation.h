// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-own.h>
#include <workerd/io/worker-generation.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class WorkerGenerationController: public jsg::Object {
 public:
  struct Owner {
    kj::Own<workerd::WorkerGeneration> generation;
    explicit Owner(kj::Own<workerd::WorkerGeneration> generation): generation(kj::mv(generation)) {}
    ~Owner() noexcept(false) {
      generation->requestStop();
    }
  };

  WorkerGenerationController(kj::Own<workerd::WorkerGeneration> generation, IoOwn<Owner> owner)
      : generation(kj::mv(generation)),
        owner(kj::mv(owner)) {}

  ~WorkerGenerationController() noexcept(false) {
    kj::mv(owner).deferGcToContext();
  }

  static jsg::Ref<WorkerGenerationController> create(
      jsg::Lock& js, jsg::Dict<uint32_t> sharedMemory);

  jsg::JsValue getSharedMemory(jsg::Lock& js, kj::String name);
  jsg::Promise<void> terminate(jsg::Lock& js);
  void terminateAfter(jsg::Lock& js, double milliseconds);
  jsg::Promise<void> whenTerminated(jsg::Lock& js);

  workerd::WorkerGeneration& getGeneration() {
    return *generation;
  }

  JSG_RESOURCE_TYPE(WorkerGenerationController) {
    JSG_METHOD(getSharedMemory);
    JSG_METHOD(terminate);
    JSG_METHOD(terminateAfter);
    JSG_METHOD(whenTerminated);

    JSG_TS_OVERRIDE({
      getSharedMemory(name: string): SharedArrayBuffer;
    });
  }

 private:
  kj::Own<workerd::WorkerGeneration> generation;
  IoOwn<Owner> owner;
  bool deadlineArmed = false;
};

}  // namespace workerd::api
