// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Differential test of the two `i18n::transcode` implementations: runs the same
// inputs through the C++ implementation (NODEJS_I18N_RUST off) and the Rust one
// (NODEJS_I18N_RUST on), and requires identical results -- the same backing
// store, byte for byte (JavaScript can observe all of it through the result's
// `buffer`), the same view into it, or the same error.

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

// Hex for short byte strings; for long ones, the length and a prefix.
kj::String describeBytes(kj::ArrayPtr<const kj::byte> bytes) {
  constexpr size_t MAX_HEX = 128;
  if (bytes.size() <= MAX_HEX) return kj::encodeHex(bytes);
  return kj::str(bytes.size(), " bytes starting ", kj::encodeHex(bytes.first(MAX_HEX)));
}

// Everything JavaScript can observe about one transcode: the whole backing
// store (including any bytes outside the view), the view's placement in it, or
// the exception thrown.
struct Outcome {
  kj::Array<kj::byte> buffer;
  size_t byteOffset = 0;
  size_t byteLength = 0;
  kj::Maybe<kj::String> exception;

  bool operator==(const Outcome& other) const {
    return buffer == other.buffer && byteOffset == other.byteOffset &&
        byteLength == other.byteLength && exception == other.exception;
  }

  kj::String toString() const {
    KJ_IF_SOME(e, exception) {
      return kj::str("threw ", e);
    }
    return kj::str(
        "buffer=", describeBytes(buffer), " byteOffset=", byteOffset, " byteLength=", byteLength);
  }
};

kj::String describeMismatch(const Outcome& expected, const Outcome& actual) {
  constexpr size_t MAX_BUFFER = 256;
  if (expected.exception != kj::none || actual.exception != kj::none ||
      (expected.buffer.size() <= MAX_BUFFER && actual.buffer.size() <= MAX_BUFFER)) {
    return kj::str("expected ", expected.toString(), "; actual ", actual.toString());
  }
  size_t common = kj::min(expected.buffer.size(), actual.buffer.size());
  size_t firstDifference = 0;
  while (firstDifference < common &&
      expected.buffer[firstDifference] == actual.buffer[firstDifference]) {
    ++firstDifference;
  }
  auto context = [&](const Outcome& outcome) {
    auto start = firstDifference - kj::min(firstDifference, size_t(16));
    auto end = kj::min(outcome.buffer.size(), firstDifference + 16);
    return kj::encodeHex(outcome.buffer.slice(start, end));
  };
  return kj::str("buffers first differ at byte ", firstDifference, "; expected ",
      expected.buffer.size(), " bytes, byteOffset=", expected.byteOffset,
      " byteLength=", expected.byteLength, " (", context(expected), "); actual ",
      actual.buffer.size(), " bytes, byteOffset=", actual.byteOffset,
      " byteLength=", actual.byteLength, " (", context(actual), ")");
}

Outcome transcodeOutcome(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> source, Encoding from, Encoding to) {
  return js.withinHandleScope([&]() -> Outcome {
    // `transcode` takes a mutable view; give it a private copy.
    auto copy = kj::heapArray(source);
    return js.tryCatch([&]() -> Outcome {
      v8::Local<v8::Uint8Array> result = i18n::transcode(js, copy, from, to);
      auto buffer = result->Buffer();
      return Outcome{
        .buffer = kj::heapArray(kj::ArrayPtr<const kj::byte>(
            static_cast<const kj::byte*>(buffer->Data()), buffer->ByteLength())),
        .byteOffset = result->ByteOffset(),
        .byteLength = result->ByteLength(),
      };
    }, [&](jsg::Value exception) -> Outcome {
      return Outcome{
        .exception = jsg::JsValue(exception.getHandle(js)).toString(js),
      };
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

// A deterministic pseudo-random generator (xorshift64).
struct Random {
  uint64_t state = 0x9e3779b97f4a7c15;

  uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }

  // A `length`-byte input, mostly interesting bytes so that ill-formed
  // sequences are common, with some arbitrary bytes among them.
  kj::Array<kj::byte> input(size_t length) {
    auto input = kj::heapArray<kj::byte>(length);
    for (auto& byte: input) {
      auto r = next();
      byte = (r & 0xff) < 192 ? INTERESTING[(r >> 8) % kj::size(INTERESTING)]
                              : static_cast<kj::byte>(r >> 8);
    }
    return input;
  }
};

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

// Appends `codePoint` to `out` as UTF-8.
void appendUtf8(kj::Vector<kj::byte>& out, char32_t codePoint) {
  if (codePoint < 0x80) {
    out.add(static_cast<kj::byte>(codePoint));
  } else if (codePoint < 0x800) {
    out.add(static_cast<kj::byte>(0xc0 | (codePoint >> 6)));
    out.add(static_cast<kj::byte>(0x80 | (codePoint & 0x3f)));
  } else if (codePoint < 0x10000) {
    out.add(static_cast<kj::byte>(0xe0 | (codePoint >> 12)));
    out.add(static_cast<kj::byte>(0x80 | ((codePoint >> 6) & 0x3f)));
    out.add(static_cast<kj::byte>(0x80 | (codePoint & 0x3f)));
  } else {
    out.add(static_cast<kj::byte>(0xf0 | (codePoint >> 18)));
    out.add(static_cast<kj::byte>(0x80 | ((codePoint >> 12) & 0x3f)));
    out.add(static_cast<kj::byte>(0x80 | ((codePoint >> 6) & 0x3f)));
    out.add(static_cast<kj::byte>(0x80 | (codePoint & 0x3f)));
  }
}

void appendUtf16Unit(kj::Vector<kj::byte>& out, char16_t unit) {
  out.add(static_cast<kj::byte>(unit & 0xff));
  out.add(static_cast<kj::byte>(unit >> 8));
}

// Appends `codePoint` to `out` as UTF-16LE.
void appendUtf16le(kj::Vector<kj::byte>& out, char32_t codePoint) {
  if (codePoint < 0x10000) {
    appendUtf16Unit(out, static_cast<char16_t>(codePoint));
  } else {
    codePoint -= 0x10000;
    appendUtf16Unit(out, static_cast<char16_t>(0xd800 | (codePoint >> 10)));
    appendUtf16Unit(out, static_cast<char16_t>(0xdc00 | (codePoint & 0x3ff)));
  }
}

using Encoder = void (*)(kj::Vector<kj::byte>&, char32_t);

kj::Array<kj::byte> encode(kj::ArrayPtr<const char32_t> codePoints, Encoder encoder) {
  kj::Vector<kj::byte> out;
  for (auto codePoint: codePoints) encoder(out, codePoint);
  return out.releaseAsArray();
}

// Lengths around the block sizes the codecs work in: simdutf's SIMD kernels
// (16 to 64 bytes per step) and ICU's conversion pivot (1024 UChars, so 2048
// bytes of UTF-16), plus lengths long enough to take many steps of each.
constexpr size_t LONG_LENGTHS[] = {15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 1023, 1024,
  1025, 2047, 2048, 2049, 3071, 3072, 3073, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536,
  65537};

// Appends inputs of several kilobytes, so that conversions cross the SIMD and
// ICU block boundaries mid-character, and so that the SIMD fast paths for
// pure-ASCII and well-formed input run over many blocks.
void addLongInputs(kj::Vector<kj::Array<kj::byte>>& corpus, Random& random) {
  // Characters of every UTF-8 and UTF-16 length, including a default
  // ignorable and a character only windows-1252 can represent.
  constexpr char32_t MIXED[] = {U'a', U'\u00e9', U'\u2615', U'\U0001F600', U'\u200B', U'\u20AC'};

  for (Encoder encoder: {appendUtf8, appendUtf16le}) {
    kj::Vector<kj::byte> text;
    for (size_t i = 0; text.size() < 65537 + 4; ++i) encoder(text, MIXED[i % kj::size(MIXED)]);
    for (auto length: LONG_LENGTHS) {
      // Cut at `length` bytes, which can split a character.
      corpus.add(kj::heapArray(text.asPtr().first(length)));
    }

    // Well-formed text, but for one ill-formed byte (UTF-8) or unpaired
    // surrogate (UTF-16) at a block boundary, or at either end.
    auto base = text.asPtr().first(8192);
    for (size_t position: {size_t(0), size_t(2), size_t(62), size_t(64), size_t(1022), size_t(1024),
           size_t(2048), size_t(4096), size_t(8188), size_t(8190)}) {
      auto input = kj::heapArray(base);
      if (encoder == appendUtf8) {
        input[position] = 0xff;
      } else {
        input[position] = 0x00;
        input[position + 1] = 0xd8;
      }
      corpus.add(kj::mv(input));
    }
  }

  for (auto length: LONG_LENGTHS) {
    // Pure ASCII.
    auto ascii = kj::heapArray<kj::byte>(length);
    for (size_t i = 0; i < length; ++i) ascii[i] = static_cast<kj::byte>(0x20 + i % 0x5f);
    corpus.add(kj::mv(ascii));

    // Only high bytes, including 0x80-0x9f, where windows-1252 departs from
    // Latin-1.
    auto high = kj::heapArray<kj::byte>(length);
    for (size_t i = 0; i < length; ++i) high[i] = static_cast<kj::byte>(0x80 + i % 0x80);
    corpus.add(kj::mv(high));
  }

  // Long pseudo-random inputs.
  for (size_t n = 0; n < 50; ++n) {
    corpus.add(random.input(static_cast<size_t>(random.next() % 20000) + 1000));
  }
}

// Supplementary code points at the edges of the ranges ICU drops, rather than
// substitutes, when a converter cannot represent them, along with the
// largest code point. (The exhaustive two-byte inputs already hold every BMP
// code point as UTF-16LE.)
constexpr char32_t SUPPLEMENTARY_EDGES[] = {0x1bc9f, 0x1bca0, 0x1bca3, 0x1bca4, 0x1d172, 0x1d173,
  0x1d17a, 0x1d17b, 0xdffff, 0xe0000, 0xe0001, 0xe0fff, 0xe1000, 0x10ffff};

// Code points per chunk of the sweeps below: not a power of two, so chunk
// boundaries fall at varied offsets within the codecs' blocks.
constexpr size_t CODE_POINTS_PER_CHUNK = 4093;

// Appends the scalar values in [first, last], every `step`th one, encoded by
// `encoder`, split into chunks.
void addCodePointSweep(kj::Vector<kj::Array<kj::byte>>& corpus,
    Encoder encoder,
    char32_t first,
    char32_t last,
    char32_t step = 1) {
  kj::Vector<kj::byte> chunk;
  size_t inChunk = 0;
  for (char32_t codePoint = first; codePoint <= last; codePoint += step) {
    if (codePoint >= 0xd800 && codePoint <= 0xdfff) continue;
    encoder(chunk, codePoint);
    if (++inChunk == CODE_POINTS_PER_CHUNK) {
      corpus.add(chunk.releaseAsArray());
      inChunk = 0;
    }
  }
  if (inChunk > 0) corpus.add(chunk.releaseAsArray());
}

// Appends Unicode text covering every code point that behaves distinctly: each
// supplementary edge case on its own, so a mismatch names it; every BMP code
// point as UTF-8 (the exhaustive two-byte inputs already hold each as
// UTF-16LE); planes 1 and 14, which hold ICU's supplementary ignorables, in
// full; and a sample of the rest of the supplementary planes.
void addUnicodeInputs(kj::Vector<kj::Array<kj::byte>>& corpus) {
  addCodePointSweep(corpus, appendUtf8, 0, 0xffff);
  for (Encoder encoder: {appendUtf8, appendUtf16le}) {
    for (auto codePoint: SUPPLEMENTARY_EDGES) {
      corpus.add(encode({codePoint}, encoder));
      corpus.add(encode({U'a', codePoint, U'b'}, encoder));
    }
    addCodePointSweep(corpus, encoder, 0x10000, 0x1ffff);
    addCodePointSweep(corpus, encoder, 0xe0000, 0xeffff);
    addCodePointSweep(corpus, encoder, 0x20000, 0x10ffff, 251);
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

  // Longer pseudo-random inputs.
  Random random;
  for (size_t n = 0; n < 5000; ++n) {
    corpus.add(random.input(static_cast<size_t>(random.next() % 64) + 5));
  }

  // Well-formed text, in each encoding that can hold it.
  constexpr char32_t TEXT[] = {U'a', U'\u00e9', U'\u2615', U'\U0001F600', U'\uFFFD', U'\uFEFF',
    U'\uFFFF', U'\u200B', U'\u20AC'};
  corpus.add(encode(kj::arrayPtr(TEXT, kj::size(TEXT)), appendUtf8));
  corpus.add(encode(kj::arrayPtr(TEXT, kj::size(TEXT)), appendUtf16le));

  addLongInputs(corpus, random);
  addUnicodeInputs(corpus);

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
                  static_cast<int>(to), i, describeBytes(corpus[i]),
                  describeMismatch(expected[i], actual));
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
