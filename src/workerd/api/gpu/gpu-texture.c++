// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-texture.h"
#include <workerd/api/gpu/gpu-texture-view.h>
#include <workerd/api/gpu/gpu-utils.h>
#include <workerd/jsg/exception.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

jsg::Ref<GPUTextureView>
GPUTexture::createView(jsg::Optional<GPUTextureViewDescriptor> descriptor) {
  wgpu::TextureViewDescriptor desc{};
  KJ_IF_SOME(d, descriptor) {
    desc.label = d.label.cStr();
    desc.format = parseTextureFormat(d.format);
    desc.dimension = parseTextureViewDimension(d.dimension);
    desc.aspect = parseTextureAspect(d.aspect.orDefault([] { return "all"_kj; }));
    desc.baseMipLevel = d.baseMipLevel.orDefault(0);
    desc.mipLevelCount = d.mipLevelCount;
    desc.baseArrayLayer = d.baseArrayLayer.orDefault(0);
    desc.arrayLayerCount = d.arrayLayerCount;
  }

  auto textureView = texture_.CreateView(&desc);
  return jsg::alloc<GPUTextureView>(kj::mv(textureView));
}

} // namespace workerd::api::gpu
