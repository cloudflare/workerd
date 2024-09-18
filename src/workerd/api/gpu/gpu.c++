// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu.h"

#include <workerd/jsg/exception.h>

#include <dawn/dawn_proc.h>

namespace workerd::api::gpu {

void initialize() {

#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
  KJ_FAIL_REQUIRE("unsupported platform for webgpu");
#endif

  // Dawn native initialization. Dawn proc allows us to point the webgpu methods
  // to different implementations such as native, wire, or our custom
  // implementation. For now we will use the native version but in the future we
  // can make use of the wire version if we separate the GPU process or a custom
  // version as a stub in tests.
  dawnProcSetProcs(&dawn::native::GetProcs());
}

GPU::GPU(): async_(kj::refcounted<AsyncRunner>(instance_.Get())) {}

kj::String parseAdapterType(wgpu::AdapterType type) {
  switch (type) {
    case wgpu::AdapterType::DiscreteGPU:
      return kj::str("Discrete GPU");
    case wgpu::AdapterType::IntegratedGPU:
      return kj::str("Integrated GPU");
    case wgpu::AdapterType::CPU:
      return kj::str("CPU");
    case wgpu::AdapterType::Unknown:
      return kj::str("Unknown");
  }
}

jsg::Promise<kj::Maybe<jsg::Ref<GPUAdapter>>> GPU::requestAdapter(
    jsg::Lock& js, jsg::Optional<GPURequestAdapterOptions> options) {

#if defined(_WIN32)
  constexpr auto defaultBackendType = wgpu::BackendType::D3D12;
#elif defined(__linux__)
  constexpr auto defaultBackendType = wgpu::BackendType::Vulkan;
#elif defined(__APPLE__)
  constexpr auto defaultBackendType = wgpu::BackendType::Metal;
#else
  KJ_UNREACHABLE;
#endif

  auto adapters = instance_.EnumerateAdapters();
  if (adapters.empty()) {
    KJ_LOG(WARNING, "no webgpu adapters found");
    return js.resolvedPromise(kj::Maybe<jsg::Ref<GPUAdapter>>(kj::none));
  }

  kj::Maybe<dawn::native::Adapter> adapter;
  for (auto& a: adapters) {
    wgpu::AdapterInfo info;
    a.GetInfo(&info);
    if (info.backendType != defaultBackendType) {
      continue;
    }

    KJ_LOG(INFO,
        kj::str("found webgpu device '", info.device, "' of type ",
            parseAdapterType(info.adapterType)));
    adapter = a;
    break;
  }

  KJ_IF_SOME(a, adapter) {
    kj::Maybe<jsg::Ref<GPUAdapter>> gpuAdapter = jsg::alloc<GPUAdapter>(a, kj::addRef(*async_));
    return js.resolvedPromise(kj::mv(gpuAdapter));
  }

  KJ_LOG(WARNING, "did not find an adapter that matched what we wanted");
  return js.resolvedPromise(kj::Maybe<jsg::Ref<GPUAdapter>>(kj::none));
}

}  // namespace workerd::api::gpu
