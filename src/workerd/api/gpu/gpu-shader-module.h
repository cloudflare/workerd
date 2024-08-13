// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-async-runner.h"
#include "gpu-utils.h"

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUCompilationMessage: public jsg::Object {
public:
  explicit GPUCompilationMessage(const WGPUCompilationMessage& m): message(m) {}

  JSG_RESOURCE_TYPE(GPUCompilationMessage) {
    JSG_READONLY_PROTOTYPE_PROPERTY(message, getMessage);
    JSG_READONLY_PROTOTYPE_PROPERTY(type, getType);
    JSG_READONLY_PROTOTYPE_PROPERTY(lineNum, getLineNum);
    JSG_READONLY_PROTOTYPE_PROPERTY(linePos, getLinePos);
    JSG_READONLY_PROTOTYPE_PROPERTY(offset, getOffset);
    JSG_READONLY_PROTOTYPE_PROPERTY(length, getLength);
  }

private:
  WGPUCompilationMessage message;

  kj::StringPtr getMessage() {
    return message.message;
  }
  GPUCompilationMessageType getType() {
    switch (message.type) {
      case WGPUCompilationMessageType_Error:
        return kj::str("error");
      case WGPUCompilationMessageType_Warning:
        return kj::str("warning");
      case WGPUCompilationMessageType_Info:
        return kj::str("info");
      default:
        KJ_UNREACHABLE
    }
  }
  double getLineNum() {
    return message.lineNum;
  }
  double getLinePos() {
    return message.linePos;
  }
  double getOffset() {
    return message.offset;
  }
  double getLength() {
    return message.length;
  }
};

class GPUCompilationInfo: public jsg::Object {
public:
  explicit GPUCompilationInfo(kj::Vector<jsg::Ref<GPUCompilationMessage>> messages)
      : messages_(kj::mv(messages)) {};
  JSG_RESOURCE_TYPE(GPUCompilationInfo) {
    JSG_READONLY_PROTOTYPE_PROPERTY(messages, getMessages);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    for (const auto& message: messages_) {
      tracker.trackField(nullptr, message);
    }
  }

private:
  kj::Vector<jsg::Ref<GPUCompilationMessage>> messages_;
  kj::ArrayPtr<jsg::Ref<GPUCompilationMessage>> getMessages() {
    return messages_.asPtr();
  };
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visitAll(messages_);
  }
};

class GPUShaderModule: public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::ShaderModule&() const {
    return shader_;
  }
  explicit GPUShaderModule(wgpu::ShaderModule s, kj::Own<AsyncRunner> async)
      : shader_(kj::mv(s)),
        async_(kj::mv(async)) {};
  JSG_RESOURCE_TYPE(GPUShaderModule) {
    JSG_METHOD(getCompilationInfo);
  }

private:
  wgpu::ShaderModule shader_;
  kj::Own<AsyncRunner> async_;
  jsg::Promise<jsg::Ref<GPUCompilationInfo>> getCompilationInfo(jsg::Lock& js);
};

struct GPUShaderModuleDescriptor {
  jsg::Optional<kj::String> label;
  kj::String code;

  JSG_STRUCT(label, code);
};

struct GPUProgrammableStage {
  jsg::Ref<GPUShaderModule> module;
  kj::String entryPoint;
  jsg::Optional<jsg::Dict<GPUPipelineConstantValue>> constants;

  JSG_STRUCT(module, entryPoint, constants);
};

}  // namespace workerd::api::gpu
