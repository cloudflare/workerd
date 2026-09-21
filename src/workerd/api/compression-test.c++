// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compression.h"

#include <kj/test.h>

namespace workerd::api {
namespace {

// No isolate: external memory adjustments are deferred and never applied.
kj::Arc<const jsg::ExternalMemoryTarget> noIsolate() {
  return kj::arc<jsg::ExternalMemoryTarget>(nullptr);
}

kj::Array<kj::byte> pattern(size_t size) {
  auto data = kj::heapArray<kj::byte>(size);
  for (size_t i = 0; i < size; i++) {
    data[i] = static_cast<kj::byte>((i * 7 + i / 251) & 0xff);
  }
  return data;
}

kj::Array<kj::byte> drain(CodecStage& stage, size_t pieceSize) {
  kj::Vector<kj::byte> out;
  auto piece = kj::heapArray<kj::byte>(pieceSize);
  while (!stage.empty()) {
    size_t n = stage.pull(piece);
    KJ_ASSERT(n > 0);
    out.addAll(piece.first(n));
  }
  return out.releaseAsArray();
}

kj::Array<kj::byte> gzipOf(kj::ArrayPtr<const kj::byte> data) {
  CodecStage stage(CodecStage::Mode::COMPRESS, "gzip"_kj, CodecStage::Flags::NONE, noIsolate());
  stage.push(data);
  stage.end();
  return drain(stage, 4096);
}

KJ_TEST("CodecStage output is pulled in production order across pump blocks") {
  // 1 MiB inflates in 64 pump iterations; pieces of 1000 bytes straddle every block boundary.
  auto data = pattern(1024 * 1024);
  auto compressed = gzipOf(data);
  KJ_EXPECT(compressed.size() < data.size());

  CodecStage stage(CodecStage::Mode::DECOMPRESS, "gzip"_kj, CodecStage::Flags::STRICT, noIsolate());
  stage.push(compressed);
  KJ_EXPECT(stage.available() == data.size());

  kj::Vector<kj::byte> out;
  auto small = kj::heapArray<kj::byte>(1000);
  for (int i = 0; i < 100; i++) {
    KJ_EXPECT(stage.pull(small) == small.size());
    out.addAll(small);
  }
  KJ_EXPECT(stage.available() == data.size() - out.size());

  // A destination larger than everything buffered takes the rest exactly.
  auto rest = kj::heapArray<kj::byte>(2 * data.size());
  size_t n = stage.pull(rest);
  KJ_EXPECT(n == data.size() - out.size());
  out.addAll(rest.first(n));
  KJ_EXPECT(stage.empty());
  KJ_EXPECT(stage.pull(rest) == 0);

  stage.end();
  KJ_EXPECT(stage.empty());
  KJ_EXPECT(out.asPtr() == data.asPtr());
}

KJ_TEST("CodecStage buffers the output of successive pushes and the flush tail in order") {
  auto data = pattern(300 * 1000);
  CodecStage compressor(
      CodecStage::Mode::COMPRESS, "deflate"_kj, CodecStage::Flags::NONE, noIsolate());
  kj::Vector<kj::byte> compressed;
  for (size_t offset = 0; offset < data.size(); offset += 1000) {
    compressor.push(data.slice(offset, offset + 1000));
    // Pulling part of the output between pushes leaves the rest in place.
    if (compressor.available() > 3) {
      kj::byte few[3];
      KJ_EXPECT(compressor.pull(few) == 3);
      compressed.addAll(kj::arrayPtr(few, 3));
    }
  }
  compressor.end();
  compressor.end();  // idempotent
  compressed.addAll(drain(compressor, 7));

  CodecStage decompressor(
      CodecStage::Mode::DECOMPRESS, "deflate"_kj, CodecStage::Flags::STRICT, noIsolate());
  decompressor.push(compressed);
  decompressor.end();
  auto restored = drain(decompressor, 64 * 1024);
  KJ_EXPECT(restored.asPtr() == data.asPtr());
}

KJ_TEST("CodecStage::clear drops the buffered output") {
  auto compressed = gzipOf(pattern(100 * 1024));
  CodecStage stage(CodecStage::Mode::DECOMPRESS, "gzip"_kj, CodecStage::Flags::NONE, noIsolate());
  stage.push(compressed);
  KJ_EXPECT(stage.available() == 100 * 1024);
  stage.clear();
  KJ_EXPECT(stage.empty());
  kj::byte buf[16];
  KJ_EXPECT(stage.pull(buf) == 0);
}

}  // namespace
}  // namespace workerd::api
