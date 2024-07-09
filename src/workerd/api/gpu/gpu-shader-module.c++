// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-shader-module.h"
#include <workerd/io/io-context.h>

namespace workerd::api::gpu {

jsg::Promise<jsg::Ref<GPUCompilationInfo>> GPUShaderModule::getCompilationInfo(jsg::Lock& js) {

  // This context object will hold information for the callback, including the
  // fulfiller to signal the caller with the result, and an async task that
  // will ensure the device's Tick() function is called periodically. It will be
  // deallocated at the end of the callback function.
  using MapAsyncContext = AsyncContext<jsg::Ref<GPUCompilationInfo>>;
  auto ctx = kj::heap<MapAsyncContext>(js, kj::addRef(*async_));
  auto promise = kj::mv(ctx->promise_);
  shader_.GetCompilationInfo(
      wgpu::CallbackMode::AllowProcessEvents,
      [ctx = kj::mv(ctx)](wgpu::CompilationInfoRequestStatus status,
                          wgpu::CompilationInfo const* compilationInfo) mutable {
        kj::Vector<jsg::Ref<GPUCompilationMessage>> messages(compilationInfo->messageCount);
        for (uint32_t i = 0; i < compilationInfo->messageCount; i++) {
          auto& msg = compilationInfo->messages[i];
          messages.add(jsg::alloc<GPUCompilationMessage>(msg));
        }

        ctx->fulfiller_->fulfill(jsg::alloc<GPUCompilationInfo>(kj::mv(messages)));
      });

  return promise;
}

} // namespace workerd::api::gpu
