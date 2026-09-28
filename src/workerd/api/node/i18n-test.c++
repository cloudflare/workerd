// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Differential test of the two `i18n::transcode` implementations: runs the same
// inputs through the C++ implementation (NODEJS_I18N_RUST off) and the Rust one
// (NODEJS_I18N_RUST on), and requires identical results -- the same bytes, the
// same backing store size (which JavaScript can observe through the result's
// `buffer`), or the same error.

#include "i18n.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/util/autogate.h>

#include <kj/encoding.h>
#include <kj/test.h>

namespace workerd::api::node {
namespace {

jsg::V8System v8System;

struct I18nContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(I18nContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(I18nIsolate, I18nContext);

constexpr Encoding ENCODINGS[] = {
  Encoding::ASCII, Encoding::LATIN1, Encoding::UTF8, Encoding::UTF16LE};

// Selects which implementation `i18n::transcode` dispatches to. Ignores
// WORKERD_ALL_AUTOGATES, which would otherwise force the gate on in the
// @all-autogates variant.
void useRust(bool enabled) {
  util::Autogate::deinitAutogate();
  if (enabled) {
    util::Autogate::initAutogateNamesForTest(
        {kj::str(util::AutogateKey::NODEJS_I18N_RUST)}, util::IgnoreAllAutogatesEnv::YES);
  } else {
    util::Autogate::initAutogateNamesForTest({}, util::IgnoreAllAutogatesEnv::YES);
  }
  KJ_ASSERT(util::Autogate::isEnabled(util::AutogateKey::NODEJS_I18N_RUST) == enabled);
}

// Everything JavaScript can observe about one transcode, rendered as a string
// so mismatches print legibly.
kj::String transcodeOutcome(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> source, Encoding from, Encoding to) {
  return js.withinHandleScope([&]() -> kj::String {
    // `transcode` takes a mutable view; give it a private copy.
    auto copy = kj::heapArray(source);
    return js.tryCatch([&]() -> kj::String {
      v8::Local<v8::Uint8Array> result = i18n::transcode(js, copy, from, to);
      auto bytes = kj::ArrayPtr<const kj::byte>(
          static_cast<const kj::byte*>(result->Buffer()->Data()) + result->ByteOffset(),
          result->ByteLength());
      return kj::str("bytes=", kj::encodeHex(bytes), " byteOffset=", result->ByteOffset(),
          " buffer.byteLength=", result->Buffer()->ByteLength());
    }, [&](jsg::Value exception) -> kj::String {
      return kj::str("threw ", jsg::JsValue(exception.getHandle(js)).toString(js));
    });
  });
}

// Bytes that exercise the interesting cases of every source encoding: ASCII
// and windows-1252 boundaries; UTF-8 lead, continuation, overlong,
// surrogate, out-of-range, and never-valid bytes; and the high bytes of
// UTF-16 surrogates and noncharacters.
constexpr kj::byte INTERESTING[] = {0x00, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbb, 0xbf,
  0xc0, 0xc2, 0xdf, 0xe0, 0xe2, 0xed, 0xef, 0xf0, 0xf4, 0xf5, 0xff, 0xd8, 0xdb, 0xdc, 0xfe, 0xfd};

// A smaller set, for exhaustive four-byte inputs: enough for every UTF-8
// sequence length, and for UTF-16 surrogate pairs in both orders.
constexpr kj::byte INTERESTING_4[] = {
  0x00, 0x41, 0x80, 0xbf, 0xc2, 0xe2, 0xed, 0xf0, 0xf4, 0xff, 0xd8, 0xdc};

// Appends every `length`-byte string over `alphabet` to `corpus`.
void addAllStrings(
    kj::Vector<kj::Array<kj::byte>>& corpus, kj::ArrayPtr<const kj::byte> alphabet, size_t length) {
  auto indices = kj::heapArray<size_t>(length);
  indices.asPtr().fill(0);
  for (;;) {
    corpus.add(KJ_MAP(i, indices) { return alphabet[i]; });
    size_t position = length;
    while (position > 0 && indices[position - 1] + 1 == alphabet.size()) {
      --position;
    }
    if (position == 0) return;
    ++indices[position - 1];
    for (size_t i = position; i < length; ++i) indices[i] = 0;
  }
}

kj::Array<kj::Array<kj::byte>> buildCorpus() {
  kj::Vector<kj::Array<kj::byte>> corpus;

  // Every input of up to two bytes.
  auto allBytes = kj::heapArray<kj::byte>(256);
  for (size_t i = 0; i < 256; ++i) allBytes[i] = static_cast<kj::byte>(i);
  for (size_t length = 0; length <= 2; ++length) addAllStrings(corpus, allBytes, length);

  // Every short input of interesting bytes.
  addAllStrings(corpus, INTERESTING, 3);
  addAllStrings(corpus, INTERESTING_4, 4);

  // Longer pseudo-random inputs, mostly interesting bytes so that ill-formed
  // sequences are common, with some arbitrary bytes among them.
  uint64_t state = 0x9e3779b97f4a7c15;
  auto next = [&]() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (size_t n = 0; n < 5000; ++n) {
    auto length = static_cast<size_t>(next() % 64) + 5;
    auto input = kj::heapArray<kj::byte>(length);
    for (auto& byte: input) {
      auto r = next();
      byte = (r & 0xff) < 192 ? INTERESTING[(r >> 8) % kj::size(INTERESTING)]
                              : static_cast<kj::byte>(r >> 8);
    }
    corpus.add(kj::mv(input));
  }

  // Well-formed text, in each encoding that can hold it.
  auto text = "a\u00e9\u2615\U0001F600\uFFFD\uFEFF\uFFFF\u200B\u20AC"_kj;
  corpus.add(kj::heapArray(text.asBytes()));
  kj::Vector<kj::byte> utf16;
  for (char16_t unit: u"a\u00e9\u2615\U0001F600\uFFFD\uFEFF\uFFFF\u200B\u20AC") {
    if (unit == 0) break;
    utf16.add(static_cast<kj::byte>(unit & 0xff));
    utf16.add(static_cast<kj::byte>(unit >> 8));
  }
  corpus.add(utf16.releaseAsArray());

  return corpus.releaseAsArray();
}

KJ_TEST("Rust and C++ transcode agree") {
  auto corpus = buildCorpus();

  jsg::test::Evaluator<I18nContext, I18nIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](I18nIsolate::Lock& isolateLock) {
    JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
        isolateLock.newContext<I18nContext>().getHandle(isolateLock), [&](jsg::Lock& js) {
      for (auto from: ENCODINGS) {
        for (auto to: ENCODINGS) {
          useRust(false);
          auto expected = KJ_MAP(source, corpus) { return transcodeOutcome(js, source, from, to); };
          useRust(true);
          size_t mismatches = 0;
          for (size_t i = 0; i < corpus.size(); ++i) {
            auto actual = transcodeOutcome(js, corpus[i], from, to);
            if (actual != expected[i] && mismatches++ < 10) {
              KJ_FAIL_EXPECT("Rust and C++ transcode disagree", static_cast<int>(from),
                  static_cast<int>(to), kj::encodeHex(corpus[i]), expected[i], actual);
            }
          }
          KJ_EXPECT(mismatches == 0, static_cast<int>(from), static_cast<int>(to), mismatches);
        }
      }
    });
  });

  util::Autogate::deinitAutogate();
}

}  // namespace
}  // namespace workerd::api::node
