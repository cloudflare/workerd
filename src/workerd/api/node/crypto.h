// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/crypto/crypto.h>
#include <workerd/api/crypto/dh.h>
#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/x509.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>

#include <ncrypto/aead.h>

namespace workerd::api::node {

class CryptoImpl final: public jsg::Object {
 public:
  // DH
  class DiffieHellmanHandle final: public jsg::Object {
   public:
    DiffieHellmanHandle(DiffieHellman dh);

    static jsg::Ref<DiffieHellmanHandle> constructor(jsg::Lock& js,
        kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
        kj::OneOf<kj::Array<kj::byte>, int> generator);

    void setPrivateKey(kj::Array<kj::byte> key);
    void setPublicKey(kj::Array<kj::byte> key);
    jsg::JsUint8Array getPublicKey(jsg::Lock& js);
    jsg::JsUint8Array getPrivateKey(jsg::Lock& js);
    jsg::JsUint8Array getGenerator(jsg::Lock& js);
    jsg::JsUint8Array getPrime(jsg::Lock& js);
    jsg::JsUint8Array computeSecret(jsg::Lock& js, kj::Array<kj::byte> key);
    jsg::JsUint8Array generateKeys(jsg::Lock& js);
    int getVerifyError();

    JSG_RESOURCE_TYPE(DiffieHellmanHandle) {
      JSG_METHOD(setPublicKey);
      JSG_METHOD(setPrivateKey);
      JSG_METHOD(getPublicKey);
      JSG_METHOD(getPrivateKey);
      JSG_METHOD(getGenerator);
      JSG_METHOD(getPrime);
      JSG_METHOD(computeSecret);
      JSG_METHOD(generateKeys);
      JSG_METHOD(getVerifyError);
    };

   private:
    DiffieHellman dh;
    int verifyError;
  };

  jsg::Ref<DiffieHellmanHandle> DiffieHellmanGroupHandle(jsg::Lock& js, kj::String name);

  jsg::JsUint8Array statelessDH(
      jsg::Lock& js, jsg::Ref<CryptoKey> privateKey, jsg::Ref<CryptoKey> publicKey);

  // Primes
  jsg::JsArrayBuffer randomPrime(jsg::Lock& js,
      uint32_t size,
      bool safe,
      jsg::Optional<kj::Array<kj::byte>> add,
      jsg::Optional<kj::Array<kj::byte>> rem);
  bool checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks);

  // Hash
  class HashHandle final: public jsg::Object {
   public:
    HashHandle(HashContext ctx): ctx(kj::mv(ctx)) {}

    static jsg::Ref<HashHandle> constructor(
        jsg::Lock& js, kj::String algorithm, kj::Maybe<uint32_t> xofLen);
    static jsg::JsUint8Array oneshot(
        jsg::Lock&, kj::String algorithm, kj::Array<kj::byte> data, kj::Maybe<uint32_t> xofLen);

    jsg::Ref<HashHandle> copy(jsg::Lock& js, kj::Maybe<uint32_t> xofLen);
    int update(kj::Array<kj::byte> data);
    jsg::JsUint8Array digest(jsg::Lock& js);

    JSG_RESOURCE_TYPE(HashHandle) {
      JSG_METHOD(update);
      JSG_METHOD(digest);
      JSG_METHOD(copy);
      JSG_STATIC_METHOD(oneshot);
    };

    void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

   private:
    HashContext ctx;
  };

  // Hmac
  class HmacHandle final: public jsg::Object {
   public:
    using KeyParam = kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>>;

    HmacHandle(HmacContext ctx): ctx(kj::mv(ctx)) {};

    static jsg::Ref<HmacHandle> constructor(jsg::Lock& js, kj::String algorithm, KeyParam key);

    // Efficiently implement one-shot HMAC that avoids multiple calls
    // across the C++/JS boundary.
    static jsg::JsUint8Array oneshot(
        jsg::Lock& js, kj::String algorithm, KeyParam key, kj::Array<kj::byte> data);

    int update(kj::Array<kj::byte> data);
    jsg::JsUint8Array digest(jsg::Lock& js);

    JSG_RESOURCE_TYPE(HmacHandle) {
      JSG_METHOD(update);
      JSG_METHOD(digest);
      JSG_STATIC_METHOD(oneshot);
    };

    void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

   private:
    HmacContext ctx;
  };

  // Hkdf
  jsg::JsArrayBuffer getHkdf(jsg::Lock& js,
      kj::String hash,
      kj::Array<const kj::byte> key,
      kj::Array<const kj::byte> salt,
      kj::Array<const kj::byte> info,
      uint32_t length);

  // Pbkdf2
  jsg::JsArrayBuffer getPbkdf(jsg::Lock& js,
      kj::Array<const kj::byte> password,
      kj::Array<const kj::byte> salt,
      uint32_t num_iterations,
      uint32_t keylen,
      kj::String name);

  // Scrypt
  jsg::JsArrayBuffer getScrypt(jsg::Lock& js,
      kj::Array<const kj::byte> password,
      kj::Array<const kj::byte> salt,
      uint32_t N,
      uint32_t r,
      uint32_t p,
      uint32_t maxmem,
      uint32_t keylen);

  // Keys
  struct KeyExportOptions {
    jsg::Optional<kj::String> type;
    jsg::Optional<kj::String> format;
    jsg::Optional<kj::String> cipher;
    jsg::Optional<kj::Array<kj::byte>> passphrase;
    JSG_STRUCT(type, format, cipher, passphrase);
  };

  struct GenerateKeyPairOptions {
    jsg::Optional<uint32_t> modulusLength;
    jsg::Optional<uint64_t> publicExponent;
    jsg::Optional<kj::String> hashAlgorithm;
    jsg::Optional<kj::String> mgf1HashAlgorithm;
    jsg::Optional<uint32_t> saltLength;
    jsg::Optional<uint32_t> divisorLength;
    jsg::Optional<kj::String> namedCurve;
    jsg::Optional<kj::Array<kj::byte>> prime;
    jsg::Optional<uint32_t> primeLength;
    jsg::Optional<uint32_t> generator;
    jsg::Optional<kj::String> groupName;
    jsg::Optional<kj::String> paramEncoding;  // one of either 'named' or 'explicit'
    jsg::Optional<KeyExportOptions> publicKeyEncoding;
    jsg::Optional<KeyExportOptions> privateKeyEncoding;

    JSG_STRUCT(modulusLength,
        publicExponent,
        hashAlgorithm,
        mgf1HashAlgorithm,
        saltLength,
        divisorLength,
        namedCurve,
        prime,
        primeLength,
        generator,
        groupName,
        paramEncoding,
        publicKeyEncoding,
        privateKeyEncoding);
  };

  struct CreateAsymmetricKeyOptions {
    kj::OneOf<jsg::JsRef<jsg::JsBufferSource>, SubtleCrypto::JsonWebKey, jsg::Ref<api::CryptoKey>>
        key;
    kj::String format;
    jsg::Optional<kj::String> type;
    jsg::Optional<jsg::JsRef<jsg::JsBufferSource>> passphrase;
    // The passphrase is only used for private keys. The format, type, and passphrase
    // options are only used if the key is a kj::Array<kj::byte>.
    JSG_STRUCT(key, format, type, passphrase);
  };

  CryptoImpl() = default;
  CryptoImpl(jsg::Lock&, const jsg::Url&) {}

  kj::OneOf<kj::String, jsg::JsArrayBuffer, SubtleCrypto::JsonWebKey> exportKey(
      jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Optional<KeyExportOptions> options);

  bool equals(jsg::Lock& js, jsg::Ref<CryptoKey> key, jsg::Ref<CryptoKey> otherKey);

  CryptoKey::AsymmetricKeyDetails getAsymmetricKeyDetail(jsg::Lock& js, jsg::Ref<CryptoKey> key);
  kj::StringPtr getAsymmetricKeyType(jsg::Lock& js, jsg::Ref<CryptoKey> key);

  jsg::Ref<CryptoKey> createSecretKey(jsg::Lock& js, jsg::JsBufferSource keyData);
  jsg::Ref<CryptoKey> createPrivateKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);
  jsg::Ref<CryptoKey> createPublicKey(jsg::Lock& js, CreateAsymmetricKeyOptions options);
  static kj::Maybe<ncrypto::EVPKeyPointer> tryGetKey(jsg::Ref<CryptoKey>& key);
  static kj::Maybe<kj::ArrayPtr<const kj::byte>> tryGetSecretKeyData(jsg::Ref<CryptoKey>& key);

  struct RsaKeyPairOptions {
    kj::String type;
    uint32_t modulusLength;
    uint32_t publicExponent;
    jsg::Optional<uint32_t> saltLength;
    jsg::Optional<kj::String> hashAlgorithm;
    jsg::Optional<kj::String> mgf1HashAlgorithm;
    JSG_STRUCT(type, modulusLength, publicExponent, saltLength, hashAlgorithm, mgf1HashAlgorithm);
  };

  struct DsaKeyPairOptions {
    uint32_t modulusLength;
    jsg::Optional<uint32_t> divisorLength;
    JSG_STRUCT(modulusLength, divisorLength);
  };

  struct EcKeyPairOptions {
    kj::String namedCurve;
    kj::String paramEncoding;
    JSG_STRUCT(namedCurve, paramEncoding);
  };

  struct EdKeyPairOptions {
    kj::String type;
    JSG_STRUCT(type);
  };

  struct DhKeyPairOptions {
    kj::OneOf<jsg::JsRef<jsg::JsBufferSource>, uint32_t, kj::String> primeOrGroup;
    jsg::Optional<uint32_t> generator;
    JSG_STRUCT(primeOrGroup, generator);
  };

  CryptoKeyPair generateRsaKeyPair(jsg::Lock& js, RsaKeyPairOptions options);
  CryptoKeyPair generateDsaKeyPair(jsg::Lock& js, DsaKeyPairOptions options);
  CryptoKeyPair generateEcKeyPair(jsg::Lock& js, EcKeyPairOptions options);
  CryptoKeyPair generateEdKeyPair(jsg::Lock& js, EdKeyPairOptions options);
  CryptoKeyPair generateDhKeyPair(jsg::Lock& js, DhKeyPairOptions options);

  // Sign/Verify
  class SignHandle final: public jsg::Object {
   public:
    SignHandle(ncrypto::EVPMDCtxPointer ctx);
    static jsg::Ref<SignHandle> constructor(jsg::Lock& js, kj::String algorithm);

    void update(jsg::Lock& js, jsg::JsBufferSource data);
    jsg::JsUint8Array sign(jsg::Lock& js,
        jsg::Ref<CryptoKey> key,
        jsg::Optional<int> rsaPadding,
        jsg::Optional<int> pssSaltLength,
        jsg::Optional<int> dsaSigEnc);

    JSG_RESOURCE_TYPE(SignHandle) {
      JSG_METHOD(update);
      JSG_METHOD(sign);
    }

   private:
    ncrypto::EVPMDCtxPointer ctx;
  };
  class VerifyHandle final: public jsg::Object {
   public:
    VerifyHandle(ncrypto::EVPMDCtxPointer ctx);
    static jsg::Ref<VerifyHandle> constructor(jsg::Lock& js, kj::String algorithm);

    void update(jsg::Lock& js, jsg::JsBufferSource data);
    bool verify(jsg::Lock& js,
        jsg::Ref<CryptoKey> key,
        jsg::JsBufferSource signature,
        jsg::Optional<int> rsaPadding,
        jsg::Optional<int> pssSaltLength,
        jsg::Optional<int> dsaSigEnc);

    JSG_RESOURCE_TYPE(VerifyHandle) {
      JSG_METHOD(update);
      JSG_METHOD(verify);
    }

   private:
    ncrypto::EVPMDCtxPointer ctx;
  };

  jsg::JsUint8Array signOneShot(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::Optional<kj::String> algorithm,
      jsg::JsBufferSource data,
      jsg::Optional<int> rsaPadding,
      jsg::Optional<int> pssSaltLength,
      jsg::Optional<int> dsaSigEnc);
  bool verifyOneShot(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::Optional<kj::String> algorithm,
      jsg::JsBufferSource data,
      jsg::JsBufferSource signature,
      jsg::Optional<int> rsaPadding,
      jsg::Optional<int> pssSaltLength,
      jsg::Optional<int> dsaSigEnc);

  // Cipher/Decipher
  enum class CipherMode { CIPHER, DECIPHER };
  class CipherHandle final: public jsg::Object {
   public:
    struct AuthenticatedInfo {
      unsigned int auth_tag_len = 0;
      unsigned int max_message_size = INT_MAX;
    };

    CipherHandle(CipherMode mode,
        ncrypto::CipherCtxPointer ctx,
        jsg::Ref<CryptoKey> key,
        kj::Array<kj::byte> iv,
        kj::Maybe<AuthenticatedInfo> maybeAuthInfo);

    static jsg::Ref<CipherHandle> construct(jsg::Lock& js,
        CipherMode mode,
        kj::StringPtr algorithm,
        ncrypto::Cipher cipher,
        jsg::Ref<CryptoKey> key,
        jsg::JsBufferSource iv,
        jsg::Optional<uint32_t> maybeAuthTagLength);

    jsg::JsUint8Array update(jsg::Lock& js, jsg::JsBufferSource data);
    jsg::JsUint8Array final(jsg::Lock& js);
    void setAAD(
        jsg::Lock& js, jsg::JsBufferSource aad, jsg::Optional<uint32_t> maybePlaintextLength);
    void setAutoPadding(jsg::Lock& js, bool autoPadding);
    void setAuthTag(jsg::Lock& js, jsg::JsBufferSource authTag);
    jsg::JsUint8Array getAuthTag(jsg::Lock& js);

    JSG_RESOURCE_TYPE(CipherHandle) {
      JSG_METHOD(update);
      JSG_METHOD(final);
      JSG_METHOD(setAAD);
      JSG_METHOD(setAutoPadding);
      JSG_METHOD(setAuthTag);
      JSG_METHOD(getAuthTag);
    };

   private:
    CipherMode mode;
    ncrypto::CipherCtxPointer ctx;
    jsg::Ref<CryptoKey> key;
    kj::Array<kj::byte> iv;
    kj::Maybe<kj::Array<kj::byte>> maybeAuthTag;
    kj::Maybe<AuthenticatedInfo> maybeAuthInfo;
    bool authTagPassed = false;
    bool pendingAuthFailed = false;
  };

  /*
  * AeadHandle implements a public interface matching CipherHandle, based on the BoringSSL-specific
  * EVP_AEAD API instead of the EVP_CIPHER API.
  *
  * It's essential to note that BoringSSL's EVP_AEAD API is *one-shot*, and will encrypt or decrypt
  * the entire ciphertext at once, and doesn't provide support for streaming operations. This is
  * for good reason, as it prevents a dangerous mistake from being made. Consider a decryption
  * operation: While it's technically possible to begin streaming chunks of decrypted data, it
  * is not safe to act on any of the data until the entire message is decrypted, validated and
  * released. If any part of the message is invalid, all of that decrypted data must be discarded.
  *
  * As a result, it's only possible to call update() once when using an AEAD algorithm, followed
  * by final(). If using the streaming interface, it is permitted to either call write() once
  * followed by end() without data; or to call end() once with a chunk of data.
  *
  * This restriction applies for certain algorithms even in the original NodeJS implementation
  * backed by OpenSSL. For example, see the note in the original documentation about CCM modes
  * of operation: <https://nodejs.org/api/crypto.html#ccm-mode>
  *
  * However, in our implementation, this restriction applies for *all* AEAD algorithms, which
  * differs from the behaviour of NodeJS.
  *
  * In principle, it's possible to allow multiple update() calls if required for a particular use
  * case, by buffering all the data supplied in memory, then invoking BoringSSL when final() is
  * called. This might not be as troubling as it sounds, as AEADs are usually used to protect
  * reasonably-sized messages used by an application, and aren't used to handle large quantities
  * of data. However, this is not yet implemented, until we see a convincing use case.
  */
  class AeadHandle final: public jsg::Object {
   public:
    struct AuthenticatedInfo {
      unsigned int auth_tag_len = 0;
      unsigned int max_message_size = INT_MAX;
    };

    AeadHandle(CipherMode mode,
        ncrypto::Aead aead,
        ncrypto::AeadCtxPointer ctx,
        jsg::Ref<CryptoKey> key,
        kj::Array<kj::byte> iv,
        kj::Maybe<AuthenticatedInfo> maybeAuthInfo);

    static jsg::Ref<AeadHandle> construct(jsg::Lock& js,
        CipherMode mode,
        kj::StringPtr algorithm,
        ncrypto::Aead aead,
        jsg::Ref<CryptoKey> key,
        jsg::JsBufferSource iv,
        jsg::Optional<uint32_t> maybeAuthTagLength);

    jsg::JsUint8Array update(jsg::Lock& js, jsg::JsBufferSource data);
    jsg::JsUint8Array final(jsg::Lock& js);
    void setAAD(
        jsg::Lock& js, jsg::JsBufferSource aad, jsg::Optional<uint32_t> maybePlaintextLength);
    void setAutoPadding(jsg::Lock&, bool);
    void setAuthTag(jsg::Lock& js, jsg::JsBufferSource authTag);
    jsg::JsUint8Array getAuthTag(jsg::Lock& js);

    JSG_RESOURCE_TYPE(AeadHandle) {
      JSG_METHOD(update);
      JSG_METHOD(final);
      JSG_METHOD(setAAD);
      JSG_METHOD(setAutoPadding);
      JSG_METHOD(setAuthTag);
      JSG_METHOD(getAuthTag);
    };

   private:
    CipherMode mode;
    ncrypto::Aead aead;
    ncrypto::AeadCtxPointer ctx;
    jsg::Ref<CryptoKey> key;
    kj::Array<kj::byte> iv;
    kj::Maybe<kj::Array<kj::byte>> maybeAuthTag;
    kj::Maybe<AuthenticatedInfo> maybeAuthInfo;
    kj::Maybe<kj::Array<kj::byte>> maybeAad;
    bool updated = false;
  };

  kj::OneOf<jsg::Ref<CipherHandle>, jsg::Ref<AeadHandle>> newHandle(jsg::Lock& js,
      kj::uint mode,
      kj::String algorithm,
      jsg::Ref<CryptoKey> key,
      jsg::JsBufferSource iv,
      jsg::Optional<uint32_t> maybeAuthTagLength);

  struct PublicPrivateCipherOptions {
    int padding;
    kj::String oaepHash;
    jsg::Optional<jsg::JsRef<jsg::JsBufferSource>> oaepLabel;
    JSG_STRUCT(padding, oaepHash, oaepLabel);
  };

  jsg::JsUint8Array publicEncrypt(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::JsBufferSource buffer,
      PublicPrivateCipherOptions options);
  jsg::JsUint8Array publicDecrypt(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::JsBufferSource buffer,
      PublicPrivateCipherOptions options);
  jsg::JsUint8Array privateEncrypt(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::JsBufferSource buffer,
      PublicPrivateCipherOptions options);
  jsg::JsUint8Array privateDecrypt(jsg::Lock& js,
      jsg::Ref<CryptoKey> key,
      jsg::JsBufferSource buffer,
      PublicPrivateCipherOptions options);

  struct CipherInfo {
    kj::String name;
    int nid;
    jsg::Optional<int> blockSize;
    jsg::Optional<int> ivLength;
    int keyLength;
    kj::String mode;  // 'cbc', 'ccm', 'cfb', 'ctr', 'ecb', 'gcm', 'ocb',
                      // 'ofb', 'stream', 'wrap', 'xts'
    JSG_STRUCT(name, nid, blockSize, ivLength, keyLength, mode)
  };

  struct GetCipherInfoOptions {
    jsg::Optional<int> keyLength;
    jsg::Optional<int> ivLength;
    JSG_STRUCT(keyLength, ivLength);
  };

  jsg::Optional<CipherInfo> getCipherInfo(
      kj::OneOf<kj::String, int> nameOrNid, GetCipherInfoOptions options);

  kj::ArrayPtr<kj::StringPtr> getCiphers();

  // SPKAC
  bool verifySpkac(kj::Array<const kj::byte> input);
  kj::Maybe<jsg::JsUint8Array> exportPublicKey(jsg::Lock& js, kj::Array<const kj::byte> input);
  kj::Maybe<jsg::JsUint8Array> exportChallenge(jsg::Lock& js, kj::Array<const kj::byte> input);

  // ECDH
  class ECDHHandle final: public jsg::Object {
   public:
    ECDHHandle(ncrypto::ECKeyPointer key);
    static jsg::Ref<ECDHHandle> constructor(jsg::Lock& js, kj::String curveName);

    static jsg::JsUint8Array convertKey(
        jsg::Lock& js, jsg::JsBufferSource key, kj::String curveName, kj::String format);

    jsg::JsUint8Array computeSecret(jsg::Lock& js, jsg::JsBufferSource otherPublicKey);
    void generateKeys();
    jsg::JsUint8Array getPrivateKey(jsg::Lock& js);
    jsg::JsUint8Array getPublicKey(jsg::Lock& js, kj::String format);
    void setPrivateKey(jsg::Lock& js, jsg::JsBufferSource key);

    JSG_RESOURCE_TYPE(ECDHHandle) {
      JSG_STATIC_METHOD(convertKey);
      JSG_METHOD(computeSecret);
      JSG_METHOD(generateKeys);
      JSG_METHOD(getPrivateKey);
      JSG_METHOD(getPublicKey);
      JSG_METHOD(setPrivateKey);
    }

   private:
    ncrypto::ECKeyPointer key_;
    const EC_GROUP* group_;
  };

  JSG_RESOURCE_TYPE(CryptoImpl) {
    // DH
    JSG_NESTED_TYPE(DiffieHellmanHandle);
    JSG_METHOD(DiffieHellmanGroupHandle);
    JSG_METHOD(statelessDH);
    // ECDH
    JSG_NESTED_TYPE(ECDHHandle);
    // Primes
    JSG_METHOD(randomPrime);
    JSG_METHOD(checkPrimeSync);
    // Hash and Hmac
    JSG_NESTED_TYPE(HashHandle);
    JSG_NESTED_TYPE(HmacHandle);
    // Hkdf
    JSG_METHOD(getHkdf);
    // Pbkdf2
    JSG_METHOD(getPbkdf);
    // Scrypt
    JSG_METHOD(getScrypt);
    // Keys
    JSG_METHOD(exportKey);
    JSG_METHOD(equals);
    JSG_METHOD(getAsymmetricKeyDetail);
    JSG_METHOD(getAsymmetricKeyType);
    JSG_METHOD(createSecretKey);
    JSG_METHOD(createPrivateKey);
    JSG_METHOD(createPublicKey);
    // Spkac
    JSG_METHOD(verifySpkac);
    JSG_METHOD(exportPublicKey);
    JSG_METHOD(exportChallenge);
    // X509
    JSG_NESTED_TYPE(X509Certificate);
    // Key generation
    JSG_METHOD(generateRsaKeyPair);
    JSG_METHOD(generateDsaKeyPair);
    JSG_METHOD(generateEcKeyPair);
    JSG_METHOD(generateEdKeyPair);
    JSG_METHOD(generateDhKeyPair);
    // Sign/Verify
    JSG_NESTED_TYPE(SignHandle);
    JSG_NESTED_TYPE(VerifyHandle);
    JSG_METHOD(signOneShot);
    JSG_METHOD(verifyOneShot);
    // Cipher/Decipher
    JSG_NESTED_TYPE(CipherHandle);
    JSG_NESTED_TYPE(AeadHandle);
    JSG_METHOD(newHandle);
    JSG_METHOD(publicEncrypt);
    JSG_METHOD(publicDecrypt);
    JSG_METHOD(privateEncrypt);
    JSG_METHOD(privateDecrypt);
    JSG_METHOD(getCipherInfo);
    JSG_METHOD(getCiphers);
  }
};

#define EW_NODE_CRYPTO_ISOLATE_TYPES                                                               \
  api::node::CryptoImpl, api::node::CryptoImpl::DiffieHellmanHandle,                               \
      api::node::CryptoImpl::HashHandle, api::node::CryptoImpl::HmacHandle,                        \
      api::node::CryptoImpl::KeyExportOptions, api::node::CryptoImpl::GenerateKeyPairOptions,      \
      api::node::CryptoImpl::CreateAsymmetricKeyOptions, api::node::CryptoImpl::RsaKeyPairOptions, \
      api::node::CryptoImpl::DsaKeyPairOptions, api::node::CryptoImpl::EcKeyPairOptions,           \
      api::node::CryptoImpl::EdKeyPairOptions, api::node::CryptoImpl::DhKeyPairOptions,            \
      api::node::CryptoImpl::SignHandle, api::node::CryptoImpl::VerifyHandle,                      \
      api::node::CryptoImpl::CipherHandle, api::node::CryptoImpl::PublicPrivateCipherOptions,      \
      api::node::CryptoImpl::CipherInfo, api::node::CryptoImpl::GetCipherInfoOptions,              \
      api::node::CryptoImpl::ECDHHandle, api::node::CryptoImpl::AeadHandle,                        \
      EW_CRYPTO_X509_ISOLATE_TYPES
}  // namespace workerd::api::node
