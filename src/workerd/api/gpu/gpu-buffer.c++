// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-buffer.h"
#include "workerd/io/io-context.h"
#include "workerd/jsg/exception.h"
#include "workerd/jsg/jsg.h"

namespace workerd::api::gpu {

GPUBuffer::GPUBuffer(jsg::Lock& js, wgpu::Buffer b, wgpu::BufferDescriptor desc,
                     wgpu::Device device, kj::Own<AsyncRunner> async)
    : buffer_(kj::mv(b)), device_(kj::mv(device)), desc_(kj::mv(desc)),
      async_(kj::mv(async)),
      detachKey_(jsg::V8Ref(js.v8Isolate, v8::Object::New(js.v8Isolate))) {

  if (desc.mappedAtCreation) {
    state_ = State::MappedAtCreation;
  }
};

v8::Local<v8::ArrayBuffer>
GPUBuffer::getMappedRange(jsg::Lock& js, jsg::Optional<GPUSize64> offset,
                          jsg::Optional<GPUSize64> size) {

  JSG_REQUIRE(state_ == State::Mapped || state_ == State::MappedAtCreation,
              TypeError, "trying to get mapped range of unmapped buffer");

  uint64_t o = offset.orDefault(0);
  uint64_t s = size.orDefault(desc_.size - o);

  uint64_t start = o;
  uint64_t end = o + s;
  for (auto& mapping : mapped_) {
    if (mapping.Intersects(start, end)) {
      JSG_FAIL_REQUIRE(TypeError, "mapping intersects with existing one");
    }
  }

  auto* ptr = (desc_.usage & wgpu::BufferUsage::MapWrite)
                  ? buffer_.GetMappedRange(o, s)
                  : const_cast<void*>(buffer_.GetConstMappedRange(o, s));

  JSG_REQUIRE(ptr, TypeError, "could not obtain mapped range");

  struct Context {
    jsg::Ref<GPUBuffer> buffer;
  };
  auto ref = JSG_THIS;
  // We're creating this context object in order for it to be available when the
  // callback is invoked. It owns a persistent reference to this GPUBuffer
  // object to ensure that it still lives while the arraybuffer is in scope.
  // This object will be deallocated when the callback finishes.
  auto ctx = new Context{ref.addRef()};
  std::shared_ptr<v8::BackingStore> backing = v8::ArrayBuffer::NewBackingStore(
      ptr, s,
      [](void* data, size_t length, void* deleter_data) {
        // we have a persistent reference to GPUBuffer so that it lives at least
        // as long as this arraybuffer
        // Note: this is invoked outside the JS isolate lock
        auto c = std::unique_ptr<Context>(static_cast<Context*>(deleter_data));
      },
      ctx);

  v8::Local<v8::ArrayBuffer> arrayBuffer =
      v8::ArrayBuffer::New(js.v8Isolate, backing);
  arrayBuffer->SetDetachKey(detachKey_.getHandle(js.v8Isolate));

  mapped_.add(Mapping{start, end,
                      jsg::V8Ref<v8::ArrayBuffer>(js.v8Isolate, arrayBuffer)});
  return arrayBuffer;
}

void GPUBuffer::DetachMappings(jsg::Lock& js) {
  for (auto& mapping : mapped_) {
    auto ab = mapping.buffer.getHandle(js.v8Isolate);

    auto res = ab->Detach(detachKey_.getHandle(js.v8Isolate));
    KJ_ASSERT(res.IsJust());
  }
  mapped_.clear();
}

void GPUBuffer::destroy(jsg::Lock& js) {
  if (state_ == State::Destroyed) {
    return;
  }

  if (state_ != State::Unmapped) {
    DetachMappings(js);
  }

  buffer_.Destroy();
  state_ = State::Destroyed;
}

void GPUBuffer::unmap(jsg::Lock& js) {
  buffer_.Unmap();

  if (state_ != State::Destroyed && state_ != State::Unmapped) {
    DetachMappings(js);
    state_ = State::Unmapped;
  }
}

jsg::Promise<void> GPUBuffer::mapAsync(GPUFlagsConstant mode,
                                       jsg::Optional<GPUSize64> offset,
                                       jsg::Optional<GPUSize64> size) {
  wgpu::MapMode md = static_cast<wgpu::MapMode>(mode);

  // we can only map unmapped buffers
  if (state_ != State::Unmapped) {
    device_.InjectError(
        wgpu::ErrorType::Validation,
        "mapAsync called on buffer that is not in the unmapped state");
    JSG_FAIL_REQUIRE(
        Error, "mapAsync called on buffer that is not in the unmapped state");
  }

  uint64_t o = offset.orDefault(0);
  uint64_t s = size.orDefault(desc_.size - o);

  struct Context {
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
    State& state;
    AsyncTask task;
  };
  auto paf = kj::newPromiseAndFulfiller<void>();
  // This context object will hold information for the callback, including the
  // fullfiller to signal the caller with the result, and an async task that
  // will ensure the device's Tick() function is called periodically. It will be
  // deallocated at the end of the callback function.
  auto ctx = new Context{kj::mv(paf.fulfiller), state_,
                         AsyncTask(kj::addRef(*async_))};

  state_ = State::MappingPending;

  buffer_.MapAsync(
      md, o, s,
      [](WGPUBufferMapAsyncStatus status, void* userdata) {
        // Note: this is invoked outside the JS isolate lock
        auto c = std::unique_ptr<Context>(static_cast<Context*>(userdata));
        c->state = State::Unmapped;

        JSG_REQUIRE(c->fulfiller->isWaiting(), TypeError,
                    "async operation has been canceled");

        switch (status) {
        case WGPUBufferMapAsyncStatus_Force32:
          KJ_UNREACHABLE
        case WGPUBufferMapAsyncStatus_Success:
          c->fulfiller->fulfill();
          c->state = State::Mapped;
          break;
        case WGPUBufferMapAsyncStatus_OffsetOutOfRange:
        case WGPUBufferMapAsyncStatus_SizeOutOfRange:
        case WGPUBufferMapAsyncStatus_ValidationError:
          c->fulfiller->reject(JSG_KJ_EXCEPTION(
              FAILED, TypeError, "validation failed or out of range"));
          break;
        case WGPUBufferMapAsyncStatus_UnmappedBeforeCallback:
        case WGPUBufferMapAsyncStatus_DestroyedBeforeCallback:
          c->fulfiller->reject(JSG_KJ_EXCEPTION(
              FAILED, TypeError, "unmapped or destroyed before callback"));
          break;
        case WGPUBufferMapAsyncStatus_Unknown:
        case WGPUBufferMapAsyncStatus_DeviceLost:
          c->fulfiller->reject(JSG_KJ_EXCEPTION(
              FAILED, TypeError, "unknown error or device lost"));
          break;
        case WGPUBufferMapAsyncStatus_MappingAlreadyPending:
          break;
        }
      },
      ctx);

  auto& context = IoContext::current();
  return context.awaitIo(kj::mv(paf.promise));
}

} // namespace workerd::api::gpu
