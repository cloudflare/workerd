// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-shader-module.h"
#include <workerd/io/io-context.h>

namespace workerd::api::gpu {

jsg::Promise<jsg::Ref<GPUCompilationInfo>> GPUShaderModule::getCompilationInfo(jsg::Lock& js) {

  struct Context {
    kj::Own<kj::PromiseFulfiller<jsg::Ref<GPUCompilationInfo>>> fulfiller;
    AsyncTask task;
  };
  auto paf = kj::newPromiseAndFulfiller<jsg::Ref<GPUCompilationInfo>>();
  // This context object will hold information for the callback, including the
  // fullfiller to signal the caller with the result, and an async task that
  // will ensure the device's Tick() function is called periodically. It will be
  // deallocated at the end of the callback function.
  auto ctx = new Context{kj::mv(paf.fulfiller), AsyncTask(kj::addRef(*async_))};
  shader_.GetCompilationInfo(
      [](WGPUCompilationInfoRequestStatus status, WGPUCompilationInfo const* compilationInfo,
         void* userdata) {
        auto c = std::unique_ptr<Context>(static_cast<Context*>(userdata));

        kj::Vector<jsg::Ref<GPUCompilationMessage>> messages(compilationInfo->messageCount);
        for (uint32_t i = 0; i < compilationInfo->messageCount; i++) {
          auto& msg = compilationInfo->messages[i];
          messages.add(jsg::alloc<GPUCompilationMessage>(msg));
        }

        c->fulfiller->fulfill(jsg::alloc<GPUCompilationInfo>(kj::mv(messages)));
      },
      ctx);

  auto& context = IoContext::current();
  return context.awaitIo(js, kj::mv(paf.promise));
}

} // namespace workerd::api::gpu
