// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "buffersource.h"
#include "jsg-test.h"

// ========================================================================================
namespace workerd::jsg::test {
namespace {

V8System v8System;

v8::Local<v8::Value> unusedBufferSourceConstructor(Lock& js, BackingStore&) {
  return v8::Undefined(js.v8Isolate);
}

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
    return BufferSource(js, BackingStore::from(js, kj::arr<kj::byte>(1, 2, 3)));
  }

  BufferSource makeArrayBuffer(jsg::Lock& js) {
    return BufferSource(js, BackingStore::alloc<v8::ArrayBuffer>(js, 3));
  }

  bool doTest(jsg::Lock& js, jsg::BufferSource buf) {
    buf.asArrayPtr()[0] = 1;
    buf.asArrayPtr()[1] = 2;
    buf.asArrayPtr()[2] = 3;
    buf.asArrayPtr()[3] = 4;
    buf.asArrayPtr()[4] = 5;
    buf.asArrayPtr()[5] = 6;
    buf.asArrayPtr()[6] = 7;
    buf.asArrayPtr()[7] = 8;

    auto ptr = buf.asArrayPtr<uint32_t>();
    KJ_ASSERT(ptr.size() == 2);
    KJ_ASSERT(ptr[0] == 0x04030201);
    KJ_ASSERT(ptr[1] == 0x08070605);
    return true;
  }

  // Regression test for AUTOVULN-CLOUDFLARE-WORKERD-17: verify that the const
  // overload of BackingStore::asArrayPtr<T>() correctly handles byteOffset as
  // bytes (not elements) for multi-byte T.
  bool testConstAsArrayPtrByteOffset(jsg::Lock& js, jsg::BufferSource buf) {
    // Write known bytes into the buffer: 12 bytes total.
    // We expect the caller to pass a Uint8Array view with byteOffset=4 and
    // byteLength=8 over a 12-byte ArrayBuffer.
    auto bytes = buf.asArrayPtr();
    KJ_ASSERT(bytes.size() == 8);
    bytes[0] = 0x01;
    bytes[1] = 0x02;
    bytes[2] = 0x03;
    bytes[3] = 0x04;
    bytes[4] = 0x05;
    bytes[5] = 0x06;
    bytes[6] = 0x07;
    bytes[7] = 0x08;

    // Now obtain a const reference and call asArrayPtr<uint32_t>() on it.
    // The view has byteOffset=4 (from the underlying ArrayBuffer) and
    // byteLength=8. The const overload must add byteOffset as bytes, not
    // as uint32_t elements, so we should get 2 uint32_t elements starting
    // at the view's data (not 4*sizeof(uint32_t)=16 bytes into the
    // backing store).
    const auto& constBuf = buf;
    auto constPtr = constBuf.asArrayPtr<uint32_t>();
    KJ_ASSERT(constPtr.size() == 2);
    KJ_ASSERT(constPtr.asBytes() == bytes.asConst());

    // Also verify the non-const overload produces the same result.
    auto mutablePtr = buf.asArrayPtr<uint32_t>();
    KJ_ASSERT(mutablePtr.size() == 2);
    KJ_ASSERT(mutablePtr.asBytes() == constPtr.asBytes());

    return true;
  }

  JSG_RESOURCE_TYPE(BufferSourceContext) {
    JSG_METHOD(takeBufferSource);
    JSG_METHOD(takeUint8Array);
    JSG_METHOD(makeBufferSource);
    JSG_METHOD(makeArrayBuffer);
    JSG_METHOD(doTest);
    JSG_METHOD(testConstAsArrayPtrByteOffset);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BufferSourceIsolate, BufferSourceContext);

KJ_TEST("BufferSource works") {
  Evaluator<BufferSourceContext, BufferSourceIsolate> e(v8System);

  // By default, a BufferSource handle is created as a DataView
  e.expectEval("makeBufferSource() instanceof Uint8Array", "boolean", "true");

  // ... but can be other types also
  e.expectEval("makeArrayBuffer() instanceof ArrayBuffer", "boolean", "true");

  e.expectEval(
      "const ab = new ArrayBuffer(9); takeBufferSource(new Uint8Array(ab, 1, 8)).byteLength === 8",
      "boolean", "true");

  e.expectEval("const ab = new ArrayBuffer(8); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Uint8Array(8); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Uint16Array(4); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Uint32Array(2); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new BigInt64Array(1); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Float16Array(4); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Float32Array(2); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new Float64Array(1); takeBufferSource(ab) === ab", "boolean", "true");

  e.expectEval("const ab = new ArrayBuffer(4); "
               "const u8 = new Uint8Array(ab, 1, 1);"
               "const u2 = takeUint8Array(u8);"
               "u8.byteLength === 0 && u2.byteLength === 1 && u2 instanceof Uint8Array && "
               "u2.buffer.byteLength === 4 && u2.byteOffset === 1 && u8 !== u2",
      "boolean", "true");

  e.expectEval("const buf = new Uint8Array(12); doTest(buf.subarray(4))", "boolean", "true");
}

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-17:
// The const overload of BackingStore::asArrayPtr<T>() must treat byteOffset as
// a byte count, not an element count. Before the fix, for multi-byte T with a
// nonzero byteOffset the const overload advanced by byteOffset * sizeof(T) bytes
// instead of byteOffset bytes, producing an out-of-bounds pointer.
KJ_TEST("BackingStore const asArrayPtr handles byteOffset correctly") {
  Evaluator<BufferSourceContext, BufferSourceIsolate> e(v8System);

  // Create a Uint8Array view at byteOffset=4 over a 12-byte ArrayBuffer.
  // The view has byteLength=8. testConstAsArrayPtrByteOffset() will call
  // the const overload of asArrayPtr<uint32_t>() and verify the pointer
  // arithmetic is correct.
  e.expectEval("const ab = new ArrayBuffer(12);"
               "const view = new Uint8Array(ab, 4, 8);"
               "testConstAsArrayPtrByteOffset(view)",
      "boolean", "true");
}

KJ_TEST("BackingStore rejects byteOffset outside backing store") {
  Evaluator<BufferSourceContext, BufferSourceIsolate> e(v8System);

  e.run([](Lock& js) {
    KJ_EXPECT_THROW(FAILED,
        BackingStore(js.allocBackingStore(8), 0, 9, 1, unusedBufferSourceConstructor, true));
  });
}

KJ_TEST("BackingStore rejects byteLength extending outside backing store") {
  Evaluator<BufferSourceContext, BufferSourceIsolate> e(v8System);

  e.run([](Lock& js) {
    KJ_EXPECT_THROW(FAILED,
        BackingStore(js.allocBackingStore(8), 2, 7, 1, unusedBufferSourceConstructor, true));
  });
}

}  // namespace
}  // namespace workerd::jsg::test
