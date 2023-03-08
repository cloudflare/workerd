// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// WebCrypto API streams, moved out of crypto.h to facilitate faster modularized build.

#include "crypto.h"
#include <workerd/jsg/jsg.h>
#include <openssl/err.h>
#include "streams/writable.h"
#include "streams/readable.h"

namespace workerd::api {

class DigestStreamSink: public WritableStreamSink {
public:
  using HashAlgorithm = SubtleCrypto::HashAlgorithm;
  using DigestContextPtr = std::unique_ptr<EVP_MD_CTX, void(*)(EVP_MD_CTX*)>;

  explicit DigestStreamSink(
      HashAlgorithm algorithm,
      kj::Own<kj::PromiseFulfiller<kj::Array<kj::byte>>> fulfiller);

  virtual ~DigestStreamSink();

  kj::Promise<void> write(const void* buffer, size_t size) override;

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override;

  kj::Promise<void> end() override;

  void abort(kj::Exception reason) override;

private:
  struct Closed {};
  using Errored = kj::Exception;

  SubtleCrypto::HashAlgorithm algorithm;
  kj::OneOf<DigestContextPtr, Closed, Errored> state;
  kj::Own<kj::PromiseFulfiller<kj::Array<kj::byte>>> fulfiller;
};

class DigestStream: public WritableStream {
  // DigestStream is a non-standard extension that provides a way of generating
  // a hash digest from streaming data. It combines Web Crypto concepts into a
  // WritableStream and is compatible with both APIs.
public:
  using HashAlgorithm = DigestStreamSink::HashAlgorithm;
  using Algorithm = kj::OneOf<kj::String, HashAlgorithm>;

  explicit DigestStream(
      HashAlgorithm algorithm,
      kj::Own<kj::PromiseFulfiller<kj::Array<kj::byte>>> fulfiller,
      jsg::Promise<kj::Array<kj::byte>> promise);

  static jsg::Ref<DigestStream> constructor(Algorithm algorithm);

  jsg::MemoizedIdentity<jsg::Promise<kj::Array<kj::byte>>>& getDigest() { return promise; }

  kj::Own<WritableStreamSink> removeSink(jsg::Lock& js) override {
    KJ_UNIMPLEMENTED("DigestStream::removeSink is not implemented");
  }

  JSG_RESOURCE_TYPE(DigestStream, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(WritableStream);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(digest, getDigest);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(digest, getDigest);
    }

    JSG_TS_OVERRIDE(extends WritableStream<ArrayBuffer | ArrayBufferView>);
  }

private:
  jsg::MemoizedIdentity<jsg::Promise<kj::Array<kj::byte>>> promise;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(promise);
  }
};

// =======================================================================================
// Crypto

class Crypto: public jsg::Object {
  // Implements the Crypto interface as prescribed by:
  // https://www.w3.org/TR/WebCryptoAPI/#crypto-interface

public:
  v8::Local<v8::ArrayBufferView> getRandomValues(v8::Local<v8::ArrayBufferView> buffer);

  kj::String randomUUID();

  jsg::Ref<SubtleCrypto> getSubtle() {
    return subtle.addRef();
  }

  JSG_RESOURCE_TYPE(Crypto, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(subtle, getSubtle);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(subtle, getSubtle);
    }
    JSG_METHOD(getRandomValues);
    JSG_METHOD(randomUUID);

    JSG_NESTED_TYPE(DigestStream);

    JSG_TS_OVERRIDE({
      getRandomValues<
        T extends
          | Int8Array
          | Uint8Array
          | Int16Array
          | Uint16Array
          | Int32Array
          | Uint32Array
          | BigInt64Array
          | BigUint64Array
      >(buffer: T): T;
    });
  }

private:
  jsg::Ref<SubtleCrypto> subtle = jsg::alloc<SubtleCrypto>();
};

#define EW_CRYPTO_ISOLATE_TYPES                       \
  api::Crypto,                                        \
  api::SubtleCrypto,                                  \
  api::CryptoKey,                                     \
  api::CryptoKeyPair,                                 \
  api::SubtleCrypto::JsonWebKey,                      \
  api::SubtleCrypto::JsonWebKey::RsaOtherPrimesInfo,  \
  api::SubtleCrypto::DeriveKeyAlgorithm,              \
  api::SubtleCrypto::EncryptAlgorithm,                \
  api::SubtleCrypto::GenerateKeyAlgorithm,            \
  api::SubtleCrypto::HashAlgorithm,                   \
  api::SubtleCrypto::ImportKeyAlgorithm,              \
  api::SubtleCrypto::SignAlgorithm,                   \
  api::CryptoKey::KeyAlgorithm,                       \
  api::CryptoKey::AesKeyAlgorithm,                    \
  api::CryptoKey::HmacKeyAlgorithm,                   \
  api::CryptoKey::RsaKeyAlgorithm,                    \
  api::CryptoKey::EllipticKeyAlgorithm,               \
  api::CryptoKey::ArbitraryKeyAlgorithm,              \
  api::DigestStream

}  // namespace workerd::api
