// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/byte-chunker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <kj/vector.h>

namespace workerd::api {
namespace {

constexpr size_t kMax = ByteChunker::kMaxChunkSize;

// Drains the chunker, checking that each chunk is at most kMaxChunkSize bytes and, for a string,
// starts a code point rather than continuing one, so that, given that the chunks together are the
// string's encoding, none ends partway through a code point. `total` is the number of bytes the
// chunker had already produced. Returns the chunks.
kj::Vector<kj::Array<kj::byte>> drain(
    jsg::Lock& js, ByteChunker& chunker, bool text, uint64_t total = 0) {
  kj::Vector<kj::Array<kj::byte>> chunks;
  while (!chunker.isDone()) {
    auto chunk = KJ_ASSERT_NONNULL(chunker.next(js));
    auto bytes = chunk.asArrayPtr();
    KJ_EXPECT(bytes.size() > 0);
    KJ_EXPECT(bytes.size() <= kMax, bytes.size());
    if (text) {
      KJ_EXPECT((bytes[0] & 0xc0) != 0x80, chunks.size());
    }
    total += bytes.size();
    chunks.add(kj::heapArray(bytes));
  }
  KJ_EXPECT(chunker.next(js) == kj::none);
  KJ_EXPECT(total == chunker.getLength());
  return chunks;
}

kj::Array<kj::byte> concat(kj::ArrayPtr<const kj::Array<kj::byte>> chunks) {
  kj::Vector<kj::byte> result;
  for (auto& chunk: chunks) {
    result.addAll(chunk);
  }
  return result.releaseAsArray();
}

// The bytes a chunker writing lone surrogates as `loneSurrogates` produces for `str`.
kj::Array<kj::byte> encodingOf(jsg::Lock& js, jsg::JsString str, LoneSurrogates loneSurrogates) {
  switch (loneSurrogates) {
    case LoneSurrogates::REPLACE:
      return kj::heapArray(str.toUSVString(js).asBytes());
    case LoneSurrogates::WTF8:
      return kj::heapArray(str.toString(js).asBytes());
  }
  KJ_UNREACHABLE;
}

kj::StringPtr modeName(LoneSurrogates loneSurrogates) {
  return loneSurrogates == LoneSurrogates::REPLACE ? "REPLACE"_kj : "WTF8"_kj;
}

// `prefix` ASCII letters followed by `rest`.
jsg::JsString stringOf(jsg::Lock& js, size_t prefix, kj::ArrayPtr<const char16_t> rest) {
  kj::Vector<char16_t> units;
  for (size_t i = 0; i < prefix; i++) {
    units.add(u'a' + i % 26);
  }
  units.addAll(rest);
  return js.str(units.asPtr());
}

KJ_TEST("ByteChunker produces a small value as one chunk") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    const char16_t text[] = {u'h', 0xe9, u'l', 0xd83d, 0xde00};
    ByteChunker chunker(js, js.str(kj::ArrayPtr<const char16_t>(text, kj::size(text))));
    KJ_EXPECT(chunker.getLength() == 8);
    KJ_EXPECT(chunker.getMaxChunkSize() == 8);
    auto chunks = drain(js, chunker, true);
    KJ_ASSERT(chunks.size() == 1);
    KJ_EXPECT(chunks[0].asPtr() == "h\xc3\xa9l\xf0\x9f\x98\x80"_kjb);

    // A lone surrogate is written as U+FFFD, as TextEncoder writes it, or as WTF-8.
    const char16_t lone[] = {u'a', 0xd800, u'b'};
    auto loneText = js.str(kj::ArrayPtr<const char16_t>(lone, kj::size(lone)));
    ByteChunker replacing(js, loneText);
    auto replaced = drain(js, replacing, true);
    KJ_ASSERT(replaced.size() == 1);
    KJ_EXPECT(replaced[0].asPtr() ==
        "a\xef\xbf\xbd"
        "b"_kjb);
    ByteChunker wtf8(js, loneText, LoneSurrogates::WTF8);
    auto encoded = drain(js, wtf8, true);
    KJ_ASSERT(encoded.size() == 1);
    KJ_EXPECT(encoded[0].asPtr() ==
        "a\xed\xa0\x80"
        "b"_kjb);
  });
}

KJ_TEST("ByteChunker ends string chunks on code point boundaries") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    struct Case {
      kj::StringPtr name;
      size_t prefix;
      kj::Array<char16_t> rest;
      size_t firstChunk;
    };
    // Each value's last code point would straddle the first chunk's end.
    Case cases[] = {
      {"two-byte Latin-1"_kj, kMax - 1, kj::heapArray<char16_t>({0xe9, u'z'}), kMax - 1},
      {"surrogate pair"_kj, kMax - 2, kj::heapArray<char16_t>({0xd83d, 0xde00, u'z'}), kMax - 2},
      // Three bytes are room for U+FFFD but not for the pair, so its lead must not be written alone.
      {"surrogate pair with three bytes left"_kj, kMax - 3,
        kj::heapArray<char16_t>({0xd83d, 0xde00, u'z'}), kMax - 3},
      {"lone lead surrogate"_kj, kMax - 2, kj::heapArray<char16_t>({0xd800, u'z'}), kMax - 2},
      // simdutf reports a lone lead surrogate with three bytes left as the output being full.
      {"lone lead surrogate with three bytes left"_kj, kMax - 3,
        kj::heapArray<char16_t>({0xd800, u'z'}), kMax},
      {"lone lead surrogate at the end"_kj, kMax - 1, kj::heapArray<char16_t>({0xd800}), kMax - 1},
      {"lone trail surrogate"_kj, kMax - 1, kj::heapArray<char16_t>({0xdc00, u'z'}), kMax - 1},
      {"pair after lone surrogate"_kj, kMax - 5,
        kj::heapArray<char16_t>({0xdc00, 0xd83d, 0xde00, u'z'}), kMax - 2},
    };
    for (auto loneSurrogates: {LoneSurrogates::REPLACE, LoneSurrogates::WTF8}) {
      auto mode = modeName(loneSurrogates);
      for (auto& c: cases) {
        auto str = stringOf(js, c.prefix, c.rest);
        ByteChunker chunker(js, str, loneSurrogates);
        auto expected = encodingOf(js, str, loneSurrogates);
        KJ_EXPECT(chunker.getLength() == expected.size(), c.name);
        auto chunks = drain(js, chunker, true);
        KJ_ASSERT(chunks.size() == 2, c.name, mode, chunks.size());
        KJ_EXPECT(chunks[0].size() == c.firstChunk, c.name, mode, chunks[0].size());
        KJ_EXPECT(concat(chunks) == expected, c.name, mode);
      }
    }
  });
}

KJ_TEST("ByteChunker splits a long string of mixed code points") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    // A 7-byte unit (é, 😀, x), whose length does not divide the chunk size, repeated past four
    // chunks, with lone surrogates mixed in.
    kj::Vector<char16_t> units;
    for (size_t i = 0; i < 80000; i++) {
      units.addAll(std::initializer_list<char16_t>{0xe9, 0xd83d, 0xde00, u'x'});
      if (i % 1000 == 0) {
        units.add(0xdfff);
      }
    }
    auto str = js.str(units.asPtr());
    for (auto loneSurrogates: {LoneSurrogates::REPLACE, LoneSurrogates::WTF8}) {
      auto mode = modeName(loneSurrogates);
      ByteChunker chunker(js, str, loneSurrogates);
      auto expected = encodingOf(js, str, loneSurrogates);
      KJ_EXPECT(chunker.getLength() == expected.size(), mode);
      auto chunks = drain(js, chunker, true);
      KJ_EXPECT(chunks.size() == expected.size() / kMax + 1, mode, chunks.size());
      KJ_EXPECT(concat(chunks) == expected, mode);
    }
  });
}

KJ_TEST("ByteChunker copies a buffer or Blob in chunks of at most 128 KiB") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto data = kj::heapArray<kj::byte>(2 * kMax + 1000);
    for (size_t i = 0; i < data.size(); i++) {
      data[i] = static_cast<kj::byte>(i * 7);
    }
    auto buffer = jsg::JsUint8Array::create(js, data);
    ByteChunker fromBuffer(js, jsg::JsBufferSource(buffer));
    ByteChunker fromBlob(js.alloc<Blob>(js, jsg::JsBufferSource(buffer), kj::str()));
    for (auto chunker: {&fromBuffer, &fromBlob}) {
      KJ_EXPECT(chunker->getLength() == data.size());
      KJ_EXPECT(chunker->getMaxChunkSize() == kMax);

      // Each chunk is a copy: changing the value after a chunk was produced doesn't change it.
      auto first = KJ_ASSERT_NONNULL(chunker->next(js));
      KJ_EXPECT(first.size() == kMax);
      auto firstCopy = kj::heapArray(first.asArrayPtr());
      buffer.asArrayPtr().first(10).fill(0);
      KJ_EXPECT(first.asArrayPtr() == firstCopy.asPtr());
      buffer.asArrayPtr().write(data.first(10));

      kj::Vector<kj::Array<kj::byte>> chunks;
      chunks.add(kj::mv(firstCopy));
      for (auto& chunk: drain(js, *chunker, false, kMax)) {
        chunks.add(kj::mv(chunk));
      }
      KJ_ASSERT(chunks.size() == 3);
      KJ_EXPECT(chunks[2].size() == 1000);
      KJ_EXPECT(concat(chunks).asPtr() == data.asPtr());
    }
  });
}

KJ_TEST("ByteChunker produces nothing for an empty value or after cancel") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;
    ByteChunker empty(js, js.str());
    KJ_EXPECT(empty.getLength() == 0);
    KJ_EXPECT(empty.isDone());
    KJ_EXPECT(empty.next(js) == kj::none);

    ByteChunker cancelled(js, js.str("hello"_kj));
    KJ_EXPECT(!cancelled.isDone());
    cancelled.cancel();
    KJ_EXPECT(cancelled.isDone());
    KJ_EXPECT(cancelled.next(js) == kj::none);
  });
}

}  // namespace
}  // namespace workerd::api
