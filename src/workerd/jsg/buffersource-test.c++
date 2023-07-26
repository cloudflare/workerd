// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "buffersource.h"

// ========================================================================================
namespace workerd::jsg::test {
namespace {

V8System v8System;

struct BufferSourceContext: public jsg::Object, public jsg::ContextGlobal {
  BufferSource takeBufferSource(BufferSource buf) {
    auto ptr = buf.asArrayPtr();
    KJ_ASSERT(!buf.isDetached());
    KJ_ASSERT(buf.size() == 8);
    KJ_ASSERT(ptr[0] == 0);

    ptr[0] = 1;
    KJ_ASSERT(ptr[0] == 1);

    return kj::mv(buf);
  }

  BufferSource takeUint8Array(Lock& js, BufferSource buf) {
    // A BufferSource that is initially attached can be detached, releasing the original
    // object, then recreated as a new instance of the same type of JS object.
    KJ_ASSERT(!buf.isDetached());
    auto handle = buf.getHandle(js);
    KJ_ASSERT(handle->IsUint8Array());
    KJ_ASSERT(handle.As<v8::Uint8Array>()->ByteLength() > 0);

    // Detaching removes the BackingStore from the BufferSource,
    // rendering the BufferSource useless.
    auto backingStore = buf.detach(js);
    KJ_ASSERT(buf.isDetached());
    KJ_ASSERT(handle.As<v8::Uint8Array>()->ByteLength() == 0);

    // We can create a new view over the same shared backing store.
    auto newView = backingStore.getTypedView<v8::DataView>();
    auto dataView = newView.createHandle(js);
    KJ_ASSERT(dataView->IsDataView());

    // We can create a new BufferSource from the detached BackingStore.
    return BufferSource(js, kj::mv(backingStore));
  }

  BufferSource makeBufferSource(jsg::Lock& js) {
    return BufferSource(js, BackingStore::from(kj::arr<kj::byte>(1, 2, 3)));
  }

  BufferSource makeArrayBuffer(jsg::Lock& js) {
    return BufferSource(js, BackingStore::alloc<v8::ArrayBuffer>(js, 3));
  }

  JSG_RESOURCE_TYPE(BufferSourceContext) {
    JSG_METHOD(takeBufferSource);
    JSG_METHOD(takeUint8Array);
    JSG_METHOD(makeBufferSource);
    JSG_METHOD(makeArrayBuffer);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BufferSourceIsolate, BufferSourceContext);

KJ_TEST("BufferSource works") {
  Evaluator<BufferSourceContext, BufferSourceIsolate> e(v8System);

  // By default, a BufferSource handle is created as a DataView
  e.expectEval(
      "makeBufferSource() instanceof Uint8Array",
      "boolean",
      "true");

  // ... but can be other types also
  e.expectEval(
      "makeArrayBuffer() instanceof ArrayBuffer",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new ArrayBuffer(9); takeBufferSource(new Uint8Array(ab, 1, 8)).byteLength === 8",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new ArrayBuffer(8); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new Uint8Array(8); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new Uint16Array(4); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new Uint32Array(2); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new BigInt64Array(1); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new Float32Array(2); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new Float64Array(1); takeBufferSource(ab) === ab",
      "boolean",
      "true");

  e.expectEval(
      "const ab = new ArrayBuffer(4); "
      "const u8 = new Uint8Array(ab, 1, 1);"
      "const u2 = takeUint8Array(u8);"
      "u8.byteLength === 0 && u2.byteLength === 1 && u2 instanceof Uint8Array && "
      "u2.buffer.byteLength === 4 && u2.byteOffset === 1 && u8 !== u2",
      "boolean",
      "true");
}

}  // namespace
}  // namespace workerd::jsg::test
