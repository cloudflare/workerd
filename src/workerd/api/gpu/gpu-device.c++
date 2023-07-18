// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-device.h"
#include "gpu-bindgroup-layout.h"
#include "gpu-bindgroup.h"
#include "gpu-buffer.h"
#include "gpu-sampler.h"
#include "gpu-utils.h"

namespace workerd::api::gpu {

jsg::Ref<GPUBuffer> GPUDevice::createBuffer(jsg::Lock &,
                                            GPUBufferDescriptor descriptor) {
  wgpu::BufferDescriptor desc{};
  desc.label = descriptor.label.cStr();
  desc.mappedAtCreation = descriptor.mappedAtCreation;
  desc.size = descriptor.size;
  desc.usage = static_cast<wgpu::BufferUsage>(descriptor.usage);
  auto buffer = device_.CreateBuffer(&desc);
  return jsg::alloc<GPUBuffer>(kj::mv(buffer), kj::mv(desc));
}

wgpu::CompareFunction parseCompareFunction(kj::StringPtr compare) {
  if (compare == "never") {
    return wgpu::CompareFunction::Never;
  }

  if (compare == "less") {
    return wgpu::CompareFunction::Less;
  }

  if (compare == "equal") {
    return wgpu::CompareFunction::Equal;
  }

  if (compare == "less-equal") {
    return wgpu::CompareFunction::LessEqual;
  }

  if (compare == "greater") {
    return wgpu::CompareFunction::Greater;
  }

  if (compare == "not-equal") {
    return wgpu::CompareFunction::NotEqual;
  }

  if (compare == "greater-equal") {
    return wgpu::CompareFunction::GreaterEqual;
  }

  if (compare == "always") {
    return wgpu::CompareFunction::Always;
  }

  KJ_FAIL_REQUIRE("unknown compare function", compare);
}

wgpu::AddressMode parseAddressMode(kj::StringPtr mode) {

  if (mode == "clamp-to-edge") {
    return wgpu::AddressMode::ClampToEdge;
  }

  if (mode == "repeat") {
    return wgpu::AddressMode::Repeat;
  }

  if (mode == "mirror-repeat") {
    return wgpu::AddressMode::MirrorRepeat;
  }

  KJ_FAIL_REQUIRE("unknown address mode", mode);
}

wgpu::FilterMode parseFilterMode(kj::StringPtr mode) {

  if (mode == "nearest") {
    return wgpu::FilterMode::Nearest;
  }

  if (mode == "linear") {
    return wgpu::FilterMode::Linear;
  }

  KJ_FAIL_REQUIRE("unknown filter mode", mode);
}

wgpu::MipmapFilterMode parseMipmapFilterMode(kj::StringPtr mode) {

  if (mode == "nearest") {
    return wgpu::MipmapFilterMode::Nearest;
  }

  if (mode == "linear") {
    return wgpu::MipmapFilterMode::Linear;
  }

  KJ_FAIL_REQUIRE("unknown mipmap filter mode", mode);
}

jsg::Ref<GPUSampler> GPUDevice::createSampler(GPUSamplerDescriptor descriptor) {
  wgpu::SamplerDescriptor desc{};

  desc.addressModeU = wgpu::AddressMode::ClampToEdge;
  desc.addressModeV = wgpu::AddressMode::ClampToEdge;
  desc.addressModeW = wgpu::AddressMode::ClampToEdge;
  desc.magFilter = wgpu::FilterMode::Nearest;
  desc.minFilter = wgpu::FilterMode::Nearest;
  desc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
  desc.lodMinClamp = 0;
  desc.lodMaxClamp = 32;
  desc.compare = parseCompareFunction(descriptor.compare);
  desc.maxAnisotropy = 1;

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  KJ_IF_MAYBE (mode, descriptor.addressModeU) {
    desc.addressModeU = parseAddressMode(*mode);
  }

  KJ_IF_MAYBE (mode, descriptor.addressModeV) {
    desc.addressModeV = parseAddressMode(*mode);
  }

  KJ_IF_MAYBE (mode, descriptor.addressModeW) {
    desc.addressModeW = parseAddressMode(*mode);
  }

  KJ_IF_MAYBE (mode, descriptor.magFilter) {
    desc.magFilter = parseFilterMode(*mode);
  }

  KJ_IF_MAYBE (mode, descriptor.minFilter) {
    desc.minFilter = parseFilterMode(*mode);
  }

  KJ_IF_MAYBE (mode, descriptor.mipmapFilter) {
    desc.mipmapFilter = parseMipmapFilterMode(*mode);
  }

  KJ_IF_MAYBE (clamp, descriptor.lodMinClamp) {
    desc.lodMinClamp = *clamp;
  }

  KJ_IF_MAYBE (clamp, descriptor.lodMaxClamp) {
    desc.lodMaxClamp = *clamp;
  }

  KJ_IF_MAYBE (anisotropy, descriptor.maxAnisotropy) {
    desc.maxAnisotropy = *anisotropy;
  }

  auto sampler = device_.CreateSampler(&desc);
  return jsg::alloc<GPUSampler>(kj::mv(sampler));
}

jsg::Ref<GPUBindGroupLayout>
GPUDevice::createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor) {
  wgpu::BindGroupLayoutDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  kj::Vector<wgpu::BindGroupLayoutEntry> layoutEntries;
  for (auto &e : descriptor.entries) {
    layoutEntries.add(parseBindGroupLayoutEntry(e));
  }
  desc.entries = layoutEntries.begin();
  desc.entryCount = layoutEntries.size();

  auto bindGroupLayout = device_.CreateBindGroupLayout(&desc);
  return jsg::alloc<GPUBindGroupLayout>(kj::mv(bindGroupLayout));
}

jsg::Ref<GPUBindGroup>
GPUDevice::createBindGroup(GPUBindGroupDescriptor descriptor) {
  wgpu::BindGroupDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  desc.layout = *descriptor.layout;

  kj::Vector<wgpu::BindGroupEntry> bindGroupEntries;
  for (auto &e : descriptor.entries) {
    bindGroupEntries.add(parseBindGroupEntry(e));
  }

  desc.entries = bindGroupEntries.begin();
  desc.entryCount = bindGroupEntries.size();

  auto bindGroup = device_.CreateBindGroup(&desc);
  return jsg::alloc<GPUBindGroup>(kj::mv(bindGroup));
}

jsg::Ref<GPUShaderModule>
GPUDevice::createShaderModule(GPUShaderModuleDescriptor descriptor) {
  wgpu::ShaderModuleDescriptor desc{};
  wgpu::ShaderModuleWGSLDescriptor wgsl_desc{};
  desc.nextInChain = &wgsl_desc;

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  wgsl_desc.code = descriptor.code.cStr();

  auto shader = device_.CreateShaderModule(&desc);
  return jsg::alloc<GPUShaderModule>(kj::mv(shader));
}

jsg::Ref<GPUPipelineLayout>
GPUDevice::createPipelineLayout(GPUPipelineLayoutDescriptor descriptor) {
  wgpu::PipelineLayoutDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  kj::Vector<wgpu::BindGroupLayout> bindGroupLayouts;
  for (auto &l: descriptor.bindGroupLayouts) {
    bindGroupLayouts.add(*l);
  }

  desc.bindGroupLayouts = bindGroupLayouts.begin();
  desc.bindGroupLayoutCount = bindGroupLayouts.size();

  auto layout = device_.CreatePipelineLayout(&desc);
  return jsg::alloc<GPUPipelineLayout>(kj::mv(layout));
}

} // namespace workerd::api::gpu
