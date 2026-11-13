// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// V8 copies a string into a caller's buffer without reporting how much it copied, and for a
// string corrupted inside the V8 sandbox it can copy less than it was asked to. These tests forge
// such a string and check that no API encodes the native memory V8 left unwritten.

#include "encoding.h"

#include <workerd/api/crypto/crypto.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <v8-internal.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

using workerd::TestFixture;

// Stands in for native memory that still holds another request's data.
constexpr kj::byte STALE = 'a';

// Fills the stack memory that the frames called next will occupy.
[[gnu::noinline]] void dirtyStack() {
  volatile kj::byte region[64 * 1024] = {};
  for (auto& b: region) b = STALE;
}

// Fills heap chunks of the given size and frees them, so that the next allocation of that size
// reuses one.
void dirtyHeap(size_t bytes) {
  kj::Array<kj::byte> chunks[16];
  for (auto& chunk: chunks) chunk = kj::heapArray<kj::byte>(bytes, STALE);
}

// A cons string of two references to `half` whose length field claims twice the characters its
// halves hold, as an attacker able to write inside the V8 sandbox can forge it. V8 copies the
// halves and returns, leaving the second half of the destination unwritten.
class ForgedString {
 public:
  ForgedString(jsg::Lock& js, v8::Local<v8::String> half)
      : str(v8::String::Concat(js.v8Isolate, half, half)),
        honestLength(str->Length()) {
    KJ_ASSERT(!jsg::JsString(str).isFlat());
    KJ_ASSERT(lengthField() == honestLength);
    lengthField() = honestLength * 2;
  }
  ~ForgedString() noexcept(false) {
    lengthField() = honestLength;
  }
  KJ_DISALLOW_COPY_AND_MOVE(ForgedString);

  operator jsg::JsString() const {
    return jsg::JsString(str);
  }

  // The number of elements a copy of the whole string is asked for.
  size_t size() const {
    return honestLength * 2;
  }

  // Checks that `copied`, the elements copied out of this string, are the `unit` its halves are
  // made of followed by zeros.
  template <typename T>
  void expectZeroTail(kj::ArrayPtr<const T> copied, T unit) const {
    KJ_ASSERT(copied.size() == size());
    for (auto c: copied.first(honestLength)) KJ_ASSERT(c == unit);
    for (auto c: copied.slice(honestLength)) KJ_ASSERT(c == 0, "stale memory was copied", c);
  }

 private:
  v8::Local<v8::String> str;
  uint32_t honestLength;

  // The handle is re-read on every access because a GC may move the string.
  uint32_t& lengthField() {
    auto object = v8::internal::ValueHelper::ValueAsAddress(*str) - v8::internal::kHeapObjectTag;
    // The map word, then the 32-bit hash field, then the length.
    return *reinterpret_cast<uint32_t*>(object + v8::internal::kApiTaggedSize + sizeof(uint32_t));
  }
};

v8::Local<v8::String> oneByteString(jsg::Lock& js, size_t length) {
  return js.str(kj::str(kj::repeat('x', length)));
}

constexpr uint16_t TWO_BYTE_UNIT = 0x0101;

v8::Local<v8::String> twoByteString(jsg::Lock& js, size_t length) {
  auto units = kj::heapArray<uint16_t>(length, TWO_BYTE_UNIT);
  return jsg::check(v8::String::NewFromTwoByte(
      js.v8Isolate, units.begin(), v8::NewStringType::kNormal, units.size()));
}

// One half short enough for its copy to be staged on the stack, one long enough for the heap.
constexpr size_t HALF_LENGTHS[] = {16, 4096};

void withIsolate(kj::Function<void(jsg::Lock&)> test) {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) { test(env.js); });
}

KJ_TEST("JsString::toArray() of a forged string holds no stale memory") {
  withIsolate([](jsg::Lock& js) {
    for (auto length: HALF_LENGTHS) {
      ForgedString forged(js, oneByteString(js, length));

      dirtyHeap(forged.size());
      auto bytes = jsg::JsString(forged).toArray<kj::byte>(js);
      forged.expectZeroTail<kj::byte>(bytes, 'x');

      dirtyHeap(forged.size() * sizeof(uint16_t));
      auto units = jsg::JsString(forged).toArray<uint16_t>(js);
      forged.expectZeroTail<uint16_t>(units, 'x');
    }
  });
}

KJ_TEST("TextEncoder.encode() of a forged one-byte string encodes no stale memory") {
  withIsolate([](jsg::Lock& js) {
    for (auto length: HALF_LENGTHS) {
      ForgedString forged(js, oneByteString(js, length));
      dirtyHeap(forged.size());
      dirtyStack();
      auto utf8 = TextEncoder::constructor(js)->encode(js, jsg::JsString(forged));
      forged.expectZeroTail<kj::byte>(utf8.asArrayPtr(), 'x');
    }
  });
}

KJ_TEST("TextEncoder.encode() of a forged two-byte string encodes no stale memory") {
  withIsolate([](jsg::Lock& js) {
    for (auto length: HALF_LENGTHS) {
      ForgedString forged(js, twoByteString(js, length));
      dirtyHeap(forged.size() * sizeof(uint16_t));
      dirtyStack();
      auto utf8 = TextEncoder::constructor(js)->encode(js, jsg::JsString(forged)).asArrayPtr();

      // Each unit of the halves encodes to two bytes, and each zero to one.
      size_t honestBytes = forged.size();
      KJ_ASSERT(utf8.size() == honestBytes + forged.size() / 2);
      for (auto b: utf8.slice(honestBytes)) KJ_ASSERT(b == 0);
    }
  });
}

KJ_TEST("DigestStream's string encoder encodes no stale memory of a forged string") {
  withIsolate([](jsg::Lock& js) {
    for (auto length: HALF_LENGTHS) {
      ForgedString forged(js, oneByteString(js, length));
      dirtyHeap(forged.size() * sizeof(char16_t));
      DigestStringEncoder encoder(DigestStringEncoding::WELL_FORMED);
      auto utf8 = encoder.encode(js, forged);
      forged.expectZeroTail<char>(utf8.asArray(), 'x');
    }
  });
}

}  // namespace
}  // namespace workerd::api
