// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto.h"
#include "impl.h"

#include <workerd/api/crypto/segment-cipher/lib.rs.h>
#include <workerd/api/streams/sync-transform.h>
#include <workerd/io/io-context.h>

#include <kj-rs/convert.h>

namespace workerd::api {

namespace {

using RustTransform = ::rust::Box<segment_cipher::SegmentTransform>;

// Runs the Rust transform (`segment-cipher/lib.rs`), giving it the IoContext's entropy for nonces.
class SegmentCipherStage final: public SyncTransformStage {
 public:
  SegmentCipherStage(RustTransform transform, kj::EntropySource& entropy)
      : transform(kj::mv(transform)),
        entropy(entropy) {}

  void push(kj::ArrayPtr<const kj::byte> input, Output& output) override {
    auto maybeException =
        kj::runCatchingExceptions([&]() { transform->push(input.as<kj_rs::Rust>(), entropy); });
    // The segments before a failure are still output.
    takeOutput(output);
    KJ_IF_SOME(exception, maybeException) {
      kj::throwFatalException(kj::mv(exception));
    }
  }

  void end(Output& output) override {
    transform->end(entropy);
    takeOutput(output);
  }

  kj::Maybe<uint64_t> getOutputLength() const override {
    return transform->output_length();
  }

  size_t retainedBytes() const override {
    return transform->retained_bytes();
  }

 private:
  RustTransform transform;
  kj::EntropySource& entropy;

  void takeOutput(Output& output) {
    auto bytes = transform->take_output();
    if (!bytes.empty()) output.add(fromRustBytes(kj::mv(bytes)));
  }
};

::rust::String toRust(kj::StringPtr string) {
  return ::rust::String(string.begin(), string.size());
}

::rust::Vec<uint8_t> toRust(jsg::Lock& js, const jsg::JsRef<jsg::JsBufferSource>& bytes) {
  return bytes.getHandle(js).asArrayPtr().as<kj_rs::RustCopy>();
}

segment_cipher::KeyInfo toRust(jsg::Lock& js, SegmentKeyInfo& info) {
  return segment_cipher::KeyInfo{
    .prefix = toRust(js, info.prefix),
    .index = toRust(info.index),
    .suffix = info.suffix.map([&](auto& suffix) { return toRust(js, suffix); }).orDefault({}),
  };
}

// Flattens the options for Rust, which validates them.
segment_cipher::CipherOptions toRust(jsg::Lock& js, SegmentCipherOptions& options) {
  auto& keys = options.keys;
  auto digestName = lookupDigestAlgorithm(getAlgorithmName(keys.hash)).first;
  segment_cipher::CipherOptions result{
    .algorithm = toRust(options.algorithm),
    .segment_size = options.segmentSize,
    .derive = toRust(keys.derive),
    .hash = toRust(digestName),
    .info = toRust(js, keys.info),
    .has_mac_info = keys.macInfo != kj::none,
    .mac_info = {},
    .has_key_bits = options.length != kj::none,
    .key_bits = options.length.orDefault(0),
    .has_first_segment = options.firstSegment != kj::none,
    .first_segment = options.firstSegment.orDefault(0),
    .has_plaintext_length = options.plaintextLength != kj::none,
    .plaintext_length = options.plaintextLength.orDefault(0),
    .has_output_offset = options.outputOffset != kj::none,
    .output_offset = options.outputOffset.orDefault(0),
    .has_output_length = options.outputLength != kj::none,
    .output_length = options.outputLength.orDefault(0),
  };
  KJ_IF_SOME(macInfo, keys.macInfo) {
    result.mac_info = toRust(js, macInfo);
  }
  return result;
}

// HKDF-Extract of the key and salt, which stays in C++ with the key. Throws an InvalidAccessError
// unless the key is an HKDF key that may derive.
ZeroOnFree extractPrk(jsg::Lock& js, SegmentKeySchedule& keys) {
  auto digest = lookupDigestAlgorithm(getAlgorithmName(keys.hash)).second;
  return ZeroOnFree(keys.key->hkdfExtract(digest, keys.salt.getHandle(js).asArrayPtr()));
}

using NewTransform = RustTransform (*)(segment_cipher::CipherOptions, ::rust::Slice<const uint8_t>);

SyncTransformStreams newSegmentStreams(
    jsg::Lock& js, SegmentCipherOptions& options, NewTransform newTransform) {
  auto prk = extractPrk(js, options.keys);
  auto transform =
      newTransform(toRust(js, options), kj::ArrayPtr<const kj::byte>(prk).as<kj_rs::Rust>());
  auto& ioContext = IoContext::current();
  // Writes are taken a segment at a time, so that an aligned write is transformed in place.
  auto highWaterMark = transform->high_water_mark();
  auto stage = kj::heap<SegmentCipherStage>(kj::mv(transform), ioContext.getEntropySource());
  return newSyncTransformStreams(js, ioContext, kj::mv(stage), highWaterMark);
}

}  // namespace

jsg::Ref<SegmentEncryptionStream> SegmentEncryptionStream::constructor(
    jsg::Lock& js, SegmentCipherOptions options) {
  auto streams = newSegmentStreams(js, options, segment_cipher::new_encryptor);
  return js.alloc<SegmentEncryptionStream>(kj::mv(streams.readable), kj::mv(streams.writable));
}

double SegmentEncryptionStream::encryptedLength(
    double plaintextLength, SegmentLayoutOptions options) {
  return segment_cipher::encrypted_length(
      plaintextLength, toRust(options.algorithm), options.segmentSize);
}

jsg::Ref<SegmentDecryptionStream> SegmentDecryptionStream::constructor(
    jsg::Lock& js, SegmentCipherOptions options) {
  auto streams = newSegmentStreams(js, options, segment_cipher::new_decryptor);
  return js.alloc<SegmentDecryptionStream>(kj::mv(streams.readable), kj::mv(streams.writable));
}

}  // namespace workerd::api
