// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUAdapterInfo: public jsg::Object {
public:
  explicit GPUAdapterInfo(wgpu::AdapterInfo);
  JSG_RESOURCE_TYPE(GPUAdapterInfo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(vendor, getVendor);
    JSG_READONLY_PROTOTYPE_PROPERTY(architecture, getArchitecture);
    JSG_READONLY_PROTOTYPE_PROPERTY(device, getDevice);
    JSG_READONLY_PROTOTYPE_PROPERTY(description, getDescription);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("vendor", vendor_);
    tracker.trackField("architecture", architecture_);
    tracker.trackField("device", device_);
    tracker.trackField("description", description_);
  }

private:
  kj::String vendor_;
  kj::String architecture_;
  kj::String device_;
  kj::String description_;
  kj::StringPtr getVendor() {
    return vendor_;
  };
  kj::StringPtr getArchitecture() {
    return architecture_;
  };
  kj::StringPtr getDevice() {
    return device_;
  };
  kj::StringPtr getDescription() {
    return description_;
  };
};

}  // namespace workerd::api::gpu
