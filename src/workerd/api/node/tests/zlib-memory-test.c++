// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/compression.h>
#include <workerd/api/node/zlib-util.h>
#include <workerd/jsg/memory.h>
#include <workerd/tests/test-fixture.h>

#include <v8-profiler.h>

#include <kj/test.h>

namespace workerd::api::node {
namespace {

kj::Own<const v8::HeapSnapshot> takeSnapshot(jsg::Lock& js) {
  static const jsg::HeapSnapshotDeleter deleter;
  return kj::Own<const v8::HeapSnapshot>(
      js.v8Isolate->GetHeapProfiler()->TakeHeapSnapshot(nullptr, nullptr, true, true), deleter);
}

bool nameIs(v8::Local<v8::Value> name, kj::StringPtr expected) {
  return kj::str(name) == expected;
}

// The size the snapshot attributes to the `workerd / <name>` node: its own size plus the native
// fields its memory info reports.
size_t snapshotSize(const v8::HeapSnapshot& snapshot, kj::StringPtr name) {
  auto expected = kj::str("workerd / ", name);
  size_t result = 0;
  for (int i = 0; i < snapshot.GetNodesCount(); i++) {
    auto node = snapshot.GetNode(i);
    if (!nameIs(node->GetName(), expected)) continue;
    size_t total = node->GetShallowSize();
    for (int c = 0; c < node->GetChildrenCount(); c++) {
      auto edge = node->GetChild(c);
      if (edge->GetToNode()->GetType() == v8::HeapGraphNode::kNative &&
          !nameIs(edge->GetName(), "cppgcshim"_kj) &&
          !nameIs(edge->GetName(), "native_to_javascript"_kj)) {
        total += edge->GetToNode()->GetShallowSize();
      }
    }
    result = kj::max(result, total);
  }
  return result;
}

constexpr size_t DATA_SIZE = 64 * 1024;

// Pseudo-random, so it does not compress and the codec buffers output.
kj::Array<kj::byte> makeData() {
  auto data = kj::heapArray<kj::byte>(DATA_SIZE);
  uint32_t s = 1;
  for (auto& b: data) {
    s = s * 1664525 + 1013904223;
    b = s >> 24;
  }
  return data;
}

template <typename Stream>
void writeData(jsg::Lock& js, Stream& stream) {
  auto output = jsg::JsUint8Array::create(js, 2 * DATA_SIZE);
  stream.template write<false>(js, Z_NO_FLUSH,
      jsg::JsBufferSource(jsg::JsUint8Array::create(js, makeData())), 0, DATA_SIZE,
      jsg::JsBufferSource(output), 0, 2 * DATA_SIZE);
}

jsg::JsArrayBufferView writeResult(jsg::Lock& js) {
  return jsg::JsArrayBuffer::create(js, 8).newUint32View(0, 2);
}

jsg::JsArrayBufferView defaultParams(jsg::Lock& js, size_t count) {
  auto params = jsg::JsArrayBuffer::create(js, count * 4).newUint32View(0, count);
  for (auto& p: params.asArrayPtr<uint32_t>()) p = static_cast<uint32_t>(-1);
  return params;
}

jsg::Function<void()> noop() {
  return jsg::Function<void()>([](jsg::Lock&) {});
}

template <typename T>
void wrap(const TestFixture::Environment& env, jsg::Ref<T>& ref) {
  auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<T>>());
  KJ_ASSERT(handler.wrap(env.js, ref.addRef())->IsObject());
}

KJ_TEST("heap snapshots report node:zlib stream state") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // Node's defaults: level 6, windowBits 15, memLevel 8.
    auto zlib = js.alloc<ZlibUtil::ZlibStream>(ZlibMode::DEFLATE, js.getExternalMemoryTarget());
    zlib->initialize(js, 15, 6, 8, Z_DEFAULT_STRATEGY, writeResult(js), noop(), kj::none);
    writeData(js, *zlib);
    wrap(env, zlib);

    using BrotliEncoder = ZlibUtil::BrotliCompressionStream<BrotliEncoderContext>;
    auto brotli = js.alloc<BrotliEncoder>(ZlibMode::BROTLI_ENCODE, js.getExternalMemoryTarget());
    KJ_ASSERT(brotli->initialize(js, defaultParams(js, 10), writeResult(js), noop()));
    writeData(js, *brotli);
    wrap(env, brotli);

    using ZstdEncoder = ZlibUtil::ZstdCompressionStream<ZstdEncoderContext>;
    auto zstd = js.alloc<ZstdEncoder>(ZlibMode::ZSTD_ENCODE, js.getExternalMemoryTarget());
    KJ_ASSERT(zstd->initialize(js, defaultParams(js, 0), writeResult(js), noop(), kj::none));
    writeData(js, *zstd);
    wrap(env, zstd);

    // Measured: zlib 285 KB, brotli 73 KB, zstd 3.7 MB of native state.
    auto snapshot = takeSnapshot(js);
    auto zlibSize = snapshotSize(*snapshot, "ZlibStream"_kj);
    auto brotliSize = snapshotSize(*snapshot, "BrotliCompressionStream"_kj);
    auto zstdSize = snapshotSize(*snapshot, "ZstdCompressionStream"_kj);
    KJ_EXPECT(zlibSize > 256 * 1024, zlibSize);
    KJ_EXPECT(brotliSize > 64 * 1024, brotliSize);
    KJ_EXPECT(zstdSize > 1024 * 1024, zstdSize);
  });
}

KJ_TEST("heap snapshots report web compression codec state") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto codec = js.alloc<CompressionCodec>(CodecStage::Mode::COMPRESS, "deflate"_kj,
        CodecStage::Flags::NONE, js.getExternalMemoryTarget());
    codec->push(jsg::JsBufferSource(jsg::JsUint8Array::create(js, makeData())));
    wrap(env, codec);

    // The deflate state (285 KB) plus the buffered output.
    auto size = snapshotSize(*takeSnapshot(js), "CompressionCodec"_kj);
    KJ_EXPECT(size > 300 * 1024, size);
  });
}

}  // namespace
}  // namespace workerd::api::node
