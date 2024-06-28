#pragma once

#include "impl.h"

namespace workerd::api {

// An algorithm-independent secret key. Used as the underlying implementation of
// things like Node.js SecretKey objects. Unlike Web Crypto keys, a secret key is
// not algorithm specific. For instance, a single secret key can be used for both
// AES and HMAC, where as Web Crypto requires a separate key for each algorithm.
class SecretKey final: public CryptoKey::Impl {
public:
  SecretKey(kj::Array<kj::byte> keyData);
  KJ_DISALLOW_COPY_AND_MOVE(SecretKey);

  kj::StringPtr getAlgorithmName() const override;
  CryptoKey::AlgorithmVariant getAlgorithm(jsg::Lock& js) const override;

  bool equals(const CryptoKey::Impl& other) const override;
  bool equals(const kj::Array<kj::byte>& other) const override;

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override;

  kj::StringPtr jsgGetMemoryName() const override { return "SecretKey"; }
  size_t jsgGetMemorySelfSize() const override { return sizeof(SecretKey); }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override;

private:
  ZeroOnFree keyData;
};

}  // namespace workerd::api
