// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUError: public jsg::Object {
 public:
  explicit GPUError(kj::String m): message_(kj::mv(m)) {};
  JSG_RESOURCE_TYPE(GPUError) {
    JSG_READONLY_PROTOTYPE_PROPERTY(message, getMessage);
  }

  kj::StringPtr getMessage() {
    return message_;
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("message", message_);
  }

 private:
  kj::String message_;
};

class GPUOutOfMemoryError: public GPUError {
 public:
  using GPUError::GPUError;
  JSG_RESOURCE_TYPE(GPUOutOfMemoryError) {
    JSG_INHERIT(GPUError);
  }
};

class GPUValidationError: public GPUError {
 public:
  using GPUError::GPUError;
  JSG_RESOURCE_TYPE(GPUValidationError) {
    JSG_INHERIT(GPUError);
  }
};

class GPUInternalError: public GPUError {
 public:
  using GPUError::GPUError;
  JSG_RESOURCE_TYPE(GPUInternalError) {
    JSG_INHERIT(GPUError);
  }
};

class GPUDeviceLostInfo: public jsg::Object {
 public:
  explicit GPUDeviceLostInfo(GPUDeviceLostReason r, kj::String m)
      : reason_(kj::mv(r)),
        message_(kj::mv(m)) {};
  JSG_RESOURCE_TYPE(GPUDeviceLostInfo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(message, getMessage);
    JSG_READONLY_PROTOTYPE_PROPERTY(reason, getReason);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("message", message_);
    tracker.trackField("reason", reason_);
  }

 private:
  GPUDeviceLostReason reason_;
  kj::String message_;
  kj::StringPtr getMessage() {
    return message_;
  }
  kj::StringPtr getReason() {
    return reason_;
  }
};

}  // namespace workerd::api::gpu
