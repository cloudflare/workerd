// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"
#include "impl.h"
#include "segment-cipher.h"

#include <workerd/api/streams/sync-transform.h>
#include <workerd/io/io-context.h>

#include <cmath>

namespace workerd::api {

namespace {

// Number.MAX_SAFE_INTEGER: larger lengths and indexes can't be represented exactly in JavaScript.
constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;

// A segment stream handles fewer than 2^32 segments, so that random 96-bit nonces stay
// collision-safe even if a key schedule is reused.
constexpr uint64_t MAX_SEGMENTS_PER_STREAM = 0xffffffffull;

uint64_t requireSafeInteger(double value, kj::StringPtr name) {
  JSG_REQUIRE(value >= 0 && value <= MAX_SAFE_INTEGER && std::floor(value) == value, RangeError,
      name, " must be a non-negative integer no greater than Number.MAX_SAFE_INTEGER.");
  return static_cast<uint64_t>(value);
}

SegmentLayout toLayout(kj::StringPtr algorithm, double segmentSize) {
  auto size = requireSafeInteger(segmentSize, "segmentSize");
  JSG_REQUIRE(size <= SegmentLayout::MAX_SEGMENT_SIZE, RangeError, "segmentSize must be between ",
      SegmentLayout::MIN_SEGMENT_SIZE, " and ", SegmentLayout::MAX_SEGMENT_SIZE, " bytes.");
  return SegmentLayout(parseSegmentAlgorithm(algorithm), static_cast<uint32_t>(size));
}

SegmentInfoTemplate toInfoTemplate(jsg::Lock& js, SegmentKeyInfo& info) {
  return SegmentInfoTemplate{
    .prefix = info.prefix.getHandle(js).copy(),
    .index = parseSegmentIndexEncoding(info.index),
    .suffix = info.suffix.map([&](auto& suffix) {
    return suffix.getHandle(js).copy();
  }).orDefault(kj::Array<kj::byte>()),
  };
}

SegmentCipher toCipher(jsg::Lock& js, SegmentCipherOptions& options) {
  auto layout = toLayout(options.algorithm, options.segmentSize);
  auto& keys = options.keys;
  JSG_REQUIRE(keys.derive == "HKDF"_kj, DOMNotSupportedError,
      "Unrecognized segment key derivation \"", keys.derive, "\"; expected \"HKDF\".");
  const EVP_MD* digest = lookupDigestAlgorithm(getAlgorithmName(keys.hash)).second;
  auto salt = keys.salt.getHandle(js);
  return SegmentCipher(layout,
      SegmentCipher::KeySchedule{
        .digest = digest,
        .prk = keys.key->hkdfExtract(digest, salt.asArrayPtr()),
        .info = toInfoTemplate(js, keys.info),
        .macInfo = keys.macInfo.map([&](SegmentKeyInfo& info) { return toInfoTemplate(js, info); }),
      },
      options.length.orDefault(256));
}

// Checks that `segmentCount` segments starting at `firstSegment` can be keyed and that the
// stream stays under its segment limit.
void requireSegmentRange(const SegmentCipher& cipher, uint64_t firstSegment, uint64_t count) {
  JSG_REQUIRE(count <= MAX_SEGMENTS_PER_STREAM, RangeError, "A segment stream can hold at most ",
      MAX_SEGMENTS_PER_STREAM, " segments.");
  // The last index, firstSegment + count - 1, must not exceed maxIndex(), which may be 2^64 - 1.
  JSG_REQUIRE(firstSegment <= cipher.maxIndex() &&
          (count == 0 || count - 1 <= cipher.maxIndex() - firstSegment),
      RangeError, "The segment indexes exceed what the key schedule's index encoding can hold.");
}

// Cuts the plaintext into segments and seals each one.
class SegmentEncryptStage final: public SyncTransformStage {
 public:
  SegmentEncryptStage(SegmentCipher cipher,
      uint64_t firstSegment,
      kj::Maybe<uint64_t> plaintextLength,
      kj::EntropySource& entropy)
      : cipher(kj::mv(cipher)),
        firstSegment(firstSegment),
        plaintextLength(plaintextLength),
        entropy(entropy),
        buffer(kj::heapArray<kj::byte>(this->cipher.getLayout().getSegmentSize())) {}

  void push(kj::ArrayPtr<const kj::byte> input, Output& output) override {
    received += input.size();
    KJ_IF_SOME(length, plaintextLength) {
      JSG_REQUIRE(received <= length, DOMOperationError, "Received more than the plaintextLength (",
          length, ") bytes of plaintext.");
    }
    size_t segmentSize = buffer.size();
    while (input.size() > 0) {
      if (buffered == 0 && input.size() >= segmentSize) {
        // A whole segment is in the input; seal it from there.
        output.add(sealNext(input.first(segmentSize)));
        input = input.slice(segmentSize);
        continue;
      }
      size_t n = kj::min(segmentSize - buffered, input.size());
      buffer.slice(buffered).write(input.first(n));
      buffered += n;
      input = input.slice(n);
      if (buffered == segmentSize) {
        output.add(sealNext(buffer));
        buffered = 0;
      }
    }
  }

  void end(Output& output) override {
    KJ_IF_SOME(length, plaintextLength) {
      JSG_REQUIRE(received == length, DOMOperationError, "Received ", received,
          " bytes of plaintext, but the plaintextLength is ", length, ".");
    }
    if (buffered > 0) {
      output.add(sealNext(buffer.first(buffered)));
      buffered = 0;
    }
  }

  kj::Maybe<uint64_t> getOutputLength() const override {
    return plaintextLength.map(
        [&](uint64_t length) { return cipher.getLayout().sealedLength(length); });
  }

 private:
  SegmentCipher cipher;
  uint64_t firstSegment;
  kj::Maybe<uint64_t> plaintextLength;
  kj::EntropySource& entropy;
  kj::Array<kj::byte> buffer;
  size_t buffered = 0;
  uint64_t received = 0;
  uint64_t segmentsSealed = 0;

  kj::Array<kj::byte> sealNext(kj::ArrayPtr<const kj::byte> plaintext) {
    requireSegmentRange(cipher, firstSegment, segmentsSealed + 1);
    return cipher.seal(firstSegment + segmentsSealed++, plaintext, entropy);
  }
};

// Splits the input into sealed segments by their known sizes, opens each one, and trims the
// plaintext to the requested range. Every segment is authenticated, including those whose
// plaintext is trimmed away.
class SegmentDecryptStage final: public SyncTransformStage {
 public:
  SegmentDecryptStage(SegmentCipher cipher,
      uint64_t firstSegment,
      uint64_t plaintextLength,
      uint64_t outputOffset,
      uint64_t outputEnd)
      : cipher(kj::mv(cipher)),
        firstSegment(firstSegment),
        plaintextLength(plaintextLength),
        outputOffset(outputOffset),
        outputEnd(outputEnd),
        expectedInput(this->cipher.getLayout().sealedLength(plaintextLength)),
        buffer(kj::heapArray<kj::byte>(this->cipher.getLayout().fullSealedSegmentSize())) {}

  void push(kj::ArrayPtr<const kj::byte> input, Output& output) override {
    while (input.size() > 0) {
      JSG_REQUIRE(received < expectedInput, DOMOperationError,
          "The input continues past the end of the last segment (byte ", expectedInput, ").");
      size_t needed = currentSealedSize();
      if (buffered == 0 && input.size() >= needed) {
        // A whole segment is in the input; open it from there.
        received += needed;
        openNext(input.first(needed), output);
        input = input.slice(needed);
        continue;
      }
      size_t n = kj::min(needed - buffered, input.size());
      buffer.slice(buffered).write(input.first(n));
      buffered += n;
      received += n;
      input = input.slice(n);
      if (buffered == needed) {
        openNext(buffer.first(needed), output);
        buffered = 0;
      }
    }
  }

  void end(Output& output) override {
    JSG_REQUIRE(received == expectedInput, DOMOperationError, "The input ended after ", received,
        " bytes, in segment ", firstSegment + segmentsOpened, "; expected ", expectedInput,
        " bytes.");
  }

  kj::Maybe<uint64_t> getOutputLength() const override {
    return outputEnd - outputOffset;
  }

 private:
  SegmentCipher cipher;
  uint64_t firstSegment;
  uint64_t plaintextLength;
  uint64_t outputOffset;
  uint64_t outputEnd;
  uint64_t expectedInput;
  kj::Array<kj::byte> buffer;
  size_t buffered = 0;
  uint64_t received = 0;
  uint64_t segmentsOpened = 0;

  // The sealed size of the next segment.
  size_t currentSealedSize() const {
    auto& layout = cipher.getLayout();
    uint64_t start = segmentsOpened * layout.getSegmentSize();
    return layout.sealedSegmentSize(kj::min(plaintextLength - start, layout.getSegmentSize()));
  }

  void openNext(kj::ArrayPtr<const kj::byte> sealed, Output& output) {
    uint64_t start = segmentsOpened * cipher.getLayout().getSegmentSize();
    auto plaintext = cipher.open(firstSegment + segmentsOpened++, sealed);
    uint64_t end = start + plaintext.size();
    // The part of [start, end) within [outputOffset, outputEnd).
    uint64_t from = kj::max(start, outputOffset);
    uint64_t to = kj::min(end, outputEnd);
    if (from >= to) return;
    if (from == start && to == end) {
      output.add(kj::mv(plaintext));
    } else {
      output.add(kj::heapArray(plaintext.slice(from - start, to - start)));
    }
  }
};

}  // namespace

jsg::Ref<SegmentEncryptionStream> SegmentEncryptionStream::constructor(
    jsg::Lock& js, SegmentCipherOptions options) {
  JSG_REQUIRE(options.outputOffset == kj::none && options.outputLength == kj::none, TypeError,
      "outputOffset and outputLength are only used by SegmentDecryptionStream.");
  auto cipher = toCipher(js, options);
  auto firstSegment = requireSafeInteger(options.firstSegment.orDefault(0), "firstSegment");
  auto plaintextLength = options.plaintextLength.map(
      [](double length) { return requireSafeInteger(length, "plaintextLength"); });
  KJ_IF_SOME(length, plaintextLength) {
    requireSegmentRange(cipher, firstSegment, cipher.getLayout().segmentCount(length));
    JSG_REQUIRE(cipher.getLayout().sealedLength(length) <= MAX_SAFE_INTEGER, RangeError,
        "The encrypted length would exceed Number.MAX_SAFE_INTEGER.");
  } else {
    requireSegmentRange(cipher, firstSegment, 0);
  }

  auto& ioContext = IoContext::current();
  auto highWaterMark = cipher.getLayout().fullSealedSegmentSize();
  auto stage = kj::heap<SegmentEncryptStage>(
      kj::mv(cipher), firstSegment, plaintextLength, ioContext.getEntropySource());
  auto streams = newSyncTransformStreams(js, ioContext, kj::mv(stage), highWaterMark);
  return js.alloc<SegmentEncryptionStream>(kj::mv(streams.readable), kj::mv(streams.writable));
}

double SegmentEncryptionStream::encryptedLength(
    double plaintextLength, SegmentLayoutOptions options) {
  auto layout = toLayout(options.algorithm, options.segmentSize);
  auto result = layout.sealedLength(requireSafeInteger(plaintextLength, "plaintextLength"));
  JSG_REQUIRE(result <= MAX_SAFE_INTEGER, RangeError,
      "The encrypted length would exceed Number.MAX_SAFE_INTEGER.");
  return static_cast<double>(result);
}

jsg::Ref<SegmentDecryptionStream> SegmentDecryptionStream::constructor(
    jsg::Lock& js, SegmentCipherOptions options) {
  auto cipher = toCipher(js, options);
  auto firstSegment = requireSafeInteger(options.firstSegment.orDefault(0), "firstSegment");
  auto plaintextLength = requireSafeInteger(
      JSG_REQUIRE_NONNULL(options.plaintextLength, TypeError,
          "SegmentDecryptionStream requires plaintextLength, so that it can detect truncation."),
      "plaintextLength");
  auto outputOffset = requireSafeInteger(options.outputOffset.orDefault(0), "outputOffset");
  JSG_REQUIRE(outputOffset <= plaintextLength, RangeError, "outputOffset (", outputOffset,
      ") is past the end of the plaintext (", plaintextLength, ").");
  uint64_t outputEnd = plaintextLength;
  KJ_IF_SOME(length, options.outputLength) {
    outputEnd = outputOffset +
        kj::min(requireSafeInteger(length, "outputLength"), plaintextLength - outputOffset);
  }
  requireSegmentRange(cipher, firstSegment, cipher.getLayout().segmentCount(plaintextLength));

  auto& ioContext = IoContext::current();
  auto highWaterMark = cipher.getLayout().getSegmentSize();
  auto stage = kj::heap<SegmentDecryptStage>(
      kj::mv(cipher), firstSegment, plaintextLength, outputOffset, outputEnd);
  auto streams = newSyncTransformStreams(js, ioContext, kj::mv(stage), highWaterMark);
  return js.alloc<SegmentDecryptionStream>(kj::mv(streams.readable), kj::mv(streams.writable));
}

}  // namespace workerd::api
