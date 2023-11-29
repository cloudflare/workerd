// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu.h"
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU_REMOTE
#include "gpu-wire-container.h"
#else
#include "gpu-native-container.h"
#endif
#include "workerd/jsg/exception.h"
#include <dawn/dawn_proc.h>

namespace workerd::api::gpu {

void initialize() {

#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
  KJ_FAIL_REQUIRE("unsupported platform for webgpu");
#endif

  // Dawn proc allows us to point the webgpu methods
  // to different implementations such as native, wire, or our custom
  // implementation.
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU_REMOTE
  // remote wire version
  dawnProcSetProcs(&dawn::wire::client::GetProcs());
#else
  // native version
  dawnProcSetProcs(&dawn::native::GetProcs());
#endif
}

GPU::GPU() {
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU_REMOTE
  dawnContainer_ = kj::heap<DawnWireContainer>();
#else
  dawnContainer_ = kj::heap<DawnNativeContainer>();
#endif

  instance_ = dawnContainer_->getInstance();
  async_ = kj::refcounted<AsyncRunner>(instance_, &*dawnContainer_);
}

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

wgpu::PowerPreference parsePowerPreference(GPUPowerPreference& pf) {

  if (pf == "low-power") {
    return wgpu::PowerPreference::LowPower;
  }

  if (pf == "high-performance") {
    return wgpu::PowerPreference::HighPerformance;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown power preference", pf);
}

jsg::Promise<kj::Maybe<jsg::Ref<GPUAdapter>>>
GPU::requestAdapter(jsg::Lock& js, jsg::Optional<GPURequestAdapterOptions> options) {

#if defined(_WIN32)
  constexpr auto defaultBackendType = wgpu::BackendType::D3D12;
#elif defined(__linux__)
  constexpr auto defaultBackendType = wgpu::BackendType::Vulkan;
#elif defined(__APPLE__)
  constexpr auto defaultBackendType = wgpu::BackendType::Metal;
#else
  KJ_UNREACHABLE;
#endif

  wgpu::RequestAdapterOptions adapterOptions{};
  // TODO(soon): don't set this for remote wire instances
  adapterOptions.backendType = defaultBackendType;

  KJ_IF_SOME(opt, options) {
    adapterOptions.powerPreference = parsePowerPreference(opt.powerPreference);
    KJ_IF_SOME(forceFallbackAdapter, opt.forceFallbackAdapter) {
      adapterOptions.forceFallbackAdapter = forceFallbackAdapter;
    }
  }

  using RequestAdapterContext = AsyncContext<kj::Maybe<jsg::Ref<GPUAdapter>>>;
  auto ctx = kj::heap<RequestAdapterContext>(js, kj::addRef(*async_));
  auto promise = kj::mv(ctx->promise_);

  instance_.RequestAdapter(
      &adapterOptions, wgpu::CallbackMode::AllowProcessEvents,
      [ctx = kj::mv(ctx), asyncRunner = kj::addRef(*async_)](
          wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, char const* message) mutable {
        wgpu::AdapterInfo info;
        switch (status) {
        case wgpu::RequestAdapterStatus::Success:
          adapter.GetInfo(&info);
          KJ_LOG(INFO, kj::str("found webgpu device '", info.device, "' of type ",
                               parseAdapterType(info.adapterType)));

          ctx->fulfiller_->fulfill(jsg::alloc<GPUAdapter>(adapter, kj::mv(asyncRunner)));
          break;
        default:
          KJ_LOG(WARNING, "did not find an adapter that matched what we wanted", (uint32_t)status,
                 message);
          ctx->fulfiller_->fulfill(kj::Maybe<jsg::Ref<GPUAdapter>>(kj::none));
        }
      });
  async_->MaybeFlush();
  return promise;
}

} // namespace workerd::api::gpu
