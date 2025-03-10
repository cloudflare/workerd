// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "crypto.h"

#include <workerd/api/crypto/digest.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/crypto/kdf.h>
#include <workerd/api/crypto/prime.h>
#include <workerd/api/crypto/spkac.h>
#include <workerd/jsg/jsg.h>

#include <ncrypto.h>

namespace workerd::api::node {

// ======================================================================================
#pragma region KDF

jsg::BufferSource CryptoImpl::getHkdf(jsg::Lock& js,
    kj::String hash,
    kj::Array<const kj::byte> key,
    kj::Array<const kj::byte> salt,
    kj::Array<const kj::byte> info,
    uint32_t length) {
  // The Node.js version of the HKDF is a bit different from the Web Crypto API
  // version. For one, the length here specifies the number of bytes, whereas
  // in Web Crypto the length is expressed in the number of bits. Second, the
  // Node.js implementation allows for a broader range of possible digest
  // algorithms whereas the Web Crypto API only allows for a few specific ones.
  // Third, the Node.js implementation enforces max size limits on the key,
  // salt, and info parameters. Fourth, the Web Crypto API relies on the key
  // being a CryptoKey object, whereas the Node.js implementation here takes a
  // raw byte array.
  auto digest = ncrypto::getDigestByName(hash.begin());

  JSG_REQUIRE_NONNULL(digest, TypeError, "Invalid Hkdf digest: ", hash);
  JSG_REQUIRE(info.size() <= INT32_MAX, RangeError, "Hkdf failed: info is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Hkdf failed: salt is too large");
  JSG_REQUIRE(key.size() <= INT32_MAX, RangeError, "Hkdf failed: key is too large");
  JSG_REQUIRE(ncrypto::checkHkdfLength(digest, length), RangeError, "Invalid Hkdf key length");

  return JSG_REQUIRE_NONNULL(api::hkdf(js, length, digest, key, salt, info), Error, "Hkdf failed");
}

jsg::BufferSource CryptoImpl::getPbkdf(jsg::Lock& js,
    kj::Array<const kj::byte> password,
    kj::Array<const kj::byte> salt,
    uint32_t num_iterations,
    uint32_t keylen,
    kj::String name) {
  // The Node.js version of the PBKDF2 is a bit different from the Web Crypto API.
  // For one, the Node.js implementation allows for a broader range of possible
  // digest algorithms whereas the Web Crypto API only allows for a few specific ones.
  // Second, the Node.js implementation enforces max size limits on the password and
  // salt parameters.
  auto digest = ncrypto::getDigestByName(name.begin());

  JSG_REQUIRE_NONNULL(
      digest, TypeError, "Invalid Pbkdf2 digest: ", name, internalDescribeOpensslErrors());
  JSG_REQUIRE(password.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: password is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Pbkdf2 failed: salt is too large");
  // Note: The user could DoS us by selecting a very high iteration count. As with the Web Crypto
  // API, intentionally limit the maximum iteration count.
  checkPbkdfLimits(js, num_iterations);

  return JSG_REQUIRE_NONNULL(
      api::pbkdf2(js, keylen, num_iterations, digest, password, salt), Error, "Pbkdf2 failed");
}

jsg::BufferSource CryptoImpl::getScrypt(jsg::Lock& js,
    kj::Array<const kj::byte> password,
    kj::Array<const kj::byte> salt,
    uint32_t N,
    uint32_t r,
    uint32_t p,
    uint32_t maxmem,
    uint32_t keylen) {
  JSG_REQUIRE(password.size() <= INT32_MAX, RangeError, "Scrypt failed: password is too large");
  JSG_REQUIRE(salt.size() <= INT32_MAX, RangeError, "Scrypt failed: salt is too large");

  return JSG_REQUIRE_NONNULL(
      api::scrypt(js, keylen, N, r, p, maxmem, password, salt), Error, "Scrypt failed");
}
#pragma endregion  // KDF

// ======================================================================================
#pragma region SPKAC

bool CryptoImpl::verifySpkac(kj::Array<const kj::byte> input) {
  return workerd::api::verifySpkac(input);
}

kj::Maybe<jsg::BufferSource> CryptoImpl::exportPublicKey(
    jsg::Lock& js, kj::Array<const kj::byte> input) {
  return workerd::api::exportPublicKey(js, input);
}

kj::Maybe<jsg::BufferSource> CryptoImpl::exportChallenge(
    jsg::Lock& js, kj::Array<const kj::byte> input) {
  return workerd::api::exportChallenge(js, input);
}
#pragma endregion  // SPKAC

// ======================================================================================
#pragma region Primes

jsg::BufferSource CryptoImpl::randomPrime(jsg::Lock& js,
    uint32_t size,
    bool safe,
    jsg::Optional<kj::Array<kj::byte>> add_buf,
    jsg::Optional<kj::Array<kj::byte>> rem_buf) {
  return workerd::api::randomPrime(js, size, safe,
      add_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }),
      rem_buf.map([](kj::Array<kj::byte>& buf) { return buf.asPtr(); }));
}

bool CryptoImpl::checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks) {
  return workerd::api::checkPrime(bufferView.asPtr(), num_checks);
}
#pragma endregion  // Primes

// ======================================================================================
#pragma region Hmac
jsg::Ref<CryptoImpl::HmacHandle> CryptoImpl::HmacHandle::constructor(
    jsg::Lock& js, kj::String algorithm, kj::OneOf<kj::Array<kj::byte>, jsg::Ref<CryptoKey>> key) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      return jsg::alloc<HmacHandle>(HmacContext(js, algorithm, key_data.asPtr()));
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      return jsg::alloc<HmacHandle>(HmacContext(js, algorithm, key->impl.get()));
    }
  }
  KJ_UNREACHABLE;
}

int CryptoImpl::HmacHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;  // This just always returns 1 no matter what.
}

jsg::BufferSource CryptoImpl::HmacHandle::digest(jsg::Lock& js) {
  return ctx.digest(js);
}

jsg::BufferSource CryptoImpl::HmacHandle::oneshot(jsg::Lock& js,
    kj::String algorithm,
    CryptoImpl::HmacHandle::KeyParam key,
    kj::Array<kj::byte> data) {
  KJ_SWITCH_ONEOF(key) {
    KJ_CASE_ONEOF(key_data, kj::Array<kj::byte>) {
      HmacContext ctx(js, algorithm, key_data.asPtr());
      ctx.update(data);
      return ctx.digest(js);
    }
    KJ_CASE_ONEOF(key, jsg::Ref<CryptoKey>) {
      HmacContext ctx(js, algorithm, key->impl.get());
      ctx.update(data);
      return ctx.digest(js);
    }
  }
  KJ_UNREACHABLE;
}

void CryptoImpl::HmacHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}
#pragma endregion  // Hmac

// ======================================================================================
#pragma region Hash
jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::constructor(
    kj::String algorithm, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(HashContext(algorithm, xofLen));
}

int CryptoImpl::HashHandle::update(kj::Array<kj::byte> data) {
  ctx.update(data);
  return 1;
}

jsg::BufferSource CryptoImpl::HashHandle::digest(jsg::Lock& js) {
  return ctx.digest(js);
}

jsg::Ref<CryptoImpl::HashHandle> CryptoImpl::HashHandle::copy(
    jsg::Lock& js, kj::Maybe<uint32_t> xofLen) {
  return jsg::alloc<HashHandle>(ctx.clone(js, kj::mv(xofLen)));
}

void CryptoImpl::HashHandle::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("digest", ctx.size());
}

jsg::BufferSource CryptoImpl::HashHandle::oneshot(
    jsg::Lock& js, kj::String algorithm, kj::Array<kj::byte> data, kj::Maybe<uint32_t> xofLen) {
  HashContext ctx(algorithm, xofLen);
  ctx.update(data);
  return ctx.digest(js);
}
#pragma endregion Hash

// ======================================================================================
#pragma region DiffieHellman

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanGroupHandle(kj::String name) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(name));
}

jsg::Ref<CryptoImpl::DiffieHellmanHandle> CryptoImpl::DiffieHellmanHandle::constructor(
    jsg::Lock& js,
    kj::OneOf<kj::Array<kj::byte>, int> sizeOrKey,
    kj::OneOf<kj::Array<kj::byte>, int> generator) {
  return jsg::alloc<DiffieHellmanHandle>(DiffieHellman(sizeOrKey, generator));
}

CryptoImpl::DiffieHellmanHandle::DiffieHellmanHandle(DiffieHellman dh): dh(kj::mv(dh)) {
  verifyError = JSG_REQUIRE_NONNULL(this->dh.check(), Error, "DiffieHellman init failed");
};

void CryptoImpl::DiffieHellmanHandle::setPrivateKey(kj::Array<kj::byte> key) {
  dh.setPrivateKey(key);
}

void CryptoImpl::DiffieHellmanHandle::setPublicKey(kj::Array<kj::byte> key) {
  dh.setPublicKey(key);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::getPublicKey(jsg::Lock& js) {
  return dh.getPublicKey(js);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::getPrivateKey(jsg::Lock& js) {
  return dh.getPrivateKey(js);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::getGenerator(jsg::Lock& js) {
  return dh.getGenerator(js);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::getPrime(jsg::Lock& js) {
  return dh.getPrime(js);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::computeSecret(
    jsg::Lock& js, kj::Array<kj::byte> key) {
  return dh.computeSecret(js, key);
}

jsg::BufferSource CryptoImpl::DiffieHellmanHandle::generateKeys(jsg::Lock& js) {
  return dh.generateKeys(js);
}

int CryptoImpl::DiffieHellmanHandle::getVerifyError() {
  return verifyError;
}
#pragma endregion  // DiffieHellman

// ======================================================================================
#pragma region SignVerify

namespace {
jsg::BackingStore signFinal(jsg::Lock& js,
    ncrypto::EVPMDCtxPointer&& mdctx,
    const ncrypto::EVPKeyPointer& pkey,
    int padding,
    jsg::Optional<int> pss_salt_len) {

  // The version of BoringSSL we use does not support DSA keys with EVP
  // When signing/verification. This may change in the future.
  JSG_REQUIRE(pkey.id() != EVP_PKEY_DSA, Error, "Signing with DSA keys is not currently supported");

  auto data = mdctx.digestFinal(mdctx.getExpectedSize());
  JSG_REQUIRE(data, Error, "Failed to generate digest");

  auto sig = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, pkey.size());
  ncrypto::Buffer<kj::byte> sig_buf{
    .data = sig.asArrayPtr().begin(),
    .len = sig.size(),
  };

  ncrypto::EVPKeyCtxPointer pkctx = pkey.newCtx();
  JSG_REQUIRE(pkctx.initForSign(), Error, "Failed to initialize signing context");

  if (pkey.isRsaVariant()) {
    std::optional<int> maybeSaltLen = std::nullopt;
    KJ_IF_SOME(len, pss_salt_len) {
      maybeSaltLen = len;
    }
    JSG_REQUIRE(ncrypto::EVPKeyCtxPointer::setRsaPadding(pkctx.get(), padding, maybeSaltLen), Error,
        "Failed to set RSA parameters for signature");
  }

  JSG_REQUIRE(pkctx.setSignatureMd(mdctx), Error, "Failed to set signature digest");
  JSG_REQUIRE(pkctx.signInto(data, &sig_buf), Error, "Failed to generate signature");

  return kj::mv(sig);
}

bool verifyFinal(jsg::Lock& js,
    ncrypto::EVPMDCtxPointer&& mdctx,
    const ncrypto::EVPKeyPointer& pkey,
    const jsg::BufferSource& signature,
    int padding,
    jsg::Optional<int> pss_salt_len) {

  // The version of BoringSSL we use does not support DSA keys with EVP
  // When signing/verification. This may change in the future.
  JSG_REQUIRE(
      pkey.id() != EVP_PKEY_DSA, Error, "Verifying with DSA keys is not currently supported");

  auto data = mdctx.digestFinal(mdctx.getExpectedSize());
  JSG_REQUIRE(data, Error, "Failed to finalize signature verification");

  ncrypto::EVPKeyCtxPointer pkctx = pkey.newCtx();
  JSG_REQUIRE(pkctx, Error, "Failed to initialize key for verification");

  const int init_ret = pkctx.initForVerify();
  JSG_REQUIRE(init_ret != -2, Error, "Failed to initialize key for verification");

  if (pkey.isRsaVariant()) {
    std::optional<int> maybeSaltLen = std::nullopt;
    KJ_IF_SOME(len, pss_salt_len) {
      maybeSaltLen = len;
    }
    JSG_REQUIRE(ncrypto::EVPKeyCtxPointer::setRsaPadding(pkctx.get(), padding, maybeSaltLen), Error,
        "Failed to set RSA parameters for signature");
  }

  JSG_REQUIRE(pkctx.setSignatureMd(mdctx), Error,
      "Failed to set digest context for signature verification");

  ncrypto::Buffer<const kj::byte> sig{
    .data = signature.asArrayPtr().begin(),
    .len = signature.size(),
  };

  return pkctx.verify(sig, data);
}

jsg::BackingStore convertSignatureToP1363(
    jsg::Lock& js, const ncrypto::EVPKeyPointer& pkey, jsg::BackingStore&& signature) {
  auto maybeRs = pkey.getBytesOfRS();
  if (!maybeRs.has_value()) return kj::mv(signature);
  unsigned int n = maybeRs.value();

  auto ret = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 2 * n);

  ncrypto::Buffer<const unsigned char> sig_buffer{
    .data = signature.asArrayPtr().begin(),
    .len = signature.size(),
  };

  if (!ncrypto::extractP1363(sig_buffer, ret.asArrayPtr().begin(), n)) {
    return kj::mv(signature);
  }

  return kj::mv(ret);
}

jsg::BackingStore convertSignatureToDER(
    jsg::Lock& js, const ncrypto::EVPKeyPointer& pkey, jsg::BackingStore&& backing) {
  auto maybeRs = pkey.getBytesOfRS();
  if (!maybeRs.has_value()) return kj::mv(backing);
  unsigned int n = maybeRs.value();

  if (backing.size() != 2 * n) {
    return jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
  }

  const kj::byte* sig_data = backing.asArrayPtr().begin();

  auto asn1_sig = ncrypto::ECDSASigPointer::New();
  JSG_REQUIRE(asn1_sig, Error, "Internal error generating signature");
  ncrypto::BignumPointer r(sig_data, n);
  JSG_REQUIRE(r, Error, "Internal error generating signature");
  ncrypto::BignumPointer s(sig_data + n, n);
  JSG_REQUIRE(s, Error, "Internal error generating signature");
  JSG_REQUIRE(asn1_sig.setParams(kj::mv(r), kj::mv(s)), Error,
      "Internal error setting signature parameters");

  auto buf = asn1_sig.encode();
  if (buf.len <= 0) [[unlikely]] {
    return jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
  }

  auto ret = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, buf.len);
  ret.asArrayPtr().copyFrom(kj::ArrayPtr<kj::byte>(buf.data, buf.len));
  return kj::mv(ret);
}

const EVP_MD* maybeGetDigest(jsg::Optional<kj::String>& maybeAlgorithm) {
  KJ_IF_SOME(alg, maybeAlgorithm) {
    auto md = ncrypto::getDigestByName(std::string_view(alg.begin(), alg.size()));
    JSG_REQUIRE(md != nullptr, Error, kj::str("Unknown digest: ", alg));
    return md;
  }
  return nullptr;
}
}  // namespace

CryptoImpl::SignHandle::SignHandle(ncrypto::EVPMDCtxPointer ctx)
    : ctx(ncrypto::EVPMDCtxPointer(ctx.release())) {}

jsg::Ref<CryptoImpl::SignHandle> CryptoImpl::SignHandle::constructor(kj::String algorithm) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  auto md = ncrypto::getDigestByName(std::string_view(algorithm.begin(), algorithm.size()));
  JSG_REQUIRE(md != nullptr, Error, kj::str("Unknown digest: ", algorithm));

  auto mdctx = ncrypto::EVPMDCtxPointer::New();
  JSG_REQUIRE(mdctx, Error, "Failed to create signing context");
  JSG_REQUIRE(mdctx.digestInit(md), Error, "Failed to initialize signing context");
  return jsg::alloc<SignHandle>(kj::mv(mdctx));
}

void CryptoImpl::SignHandle::update(jsg::Lock& js, jsg::BufferSource data) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  JSG_REQUIRE(ctx, Error, "Signing context has already been finalized");
  auto ptr = data.asArrayPtr();
  ncrypto::Buffer<const void> buf{
    .data = ptr.begin(),
    .len = ptr.size(),
  };
  JSG_REQUIRE(ctx.digestUpdate(buf), Error, "Failed to update signing context");
}

jsg::BufferSource CryptoImpl::SignHandle::sign(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<int> rsaPadding,
    jsg::Optional<int> pssSaltLength,
    jsg::Optional<int> dsaSigEnc) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  JSG_REQUIRE(ctx, Error, "Signing context has already been finalized");

  const auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "Invalid key for sign operation");
  JSG_REQUIRE(pkey.validateDsaParameters(), Error, "Invalid DSA parameters");

  // There's a bug in ncrypto that doesn't clear the EVPMDCtxPointer when
  // moved with kj::mv so instead we release and wrap again.
  auto backing = signFinal(js, ncrypto::EVPMDCtxPointer(ctx.release()), pkey,
      rsaPadding.orDefault(pkey.getDefaultSignPadding()), pssSaltLength);

  KJ_IF_SOME(enc, dsaSigEnc) {
    static constexpr unsigned kP1363 = 1;
    JSG_REQUIRE(enc <= kP1363 && enc >= 0, Error, "Invalid DSA signature encoding");
    if (enc == kP1363) {
      backing = convertSignatureToP1363(js, pkey, kj::mv(backing));
    }
  }

  return jsg::BufferSource(js, kj::mv(backing));
}

CryptoImpl::VerifyHandle::VerifyHandle(ncrypto::EVPMDCtxPointer ctx)
    : ctx(ncrypto::EVPMDCtxPointer(ctx.release())) {}

jsg::Ref<CryptoImpl::VerifyHandle> CryptoImpl::VerifyHandle::constructor(kj::String algorithm) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  auto md = ncrypto::getDigestByName(std::string_view(algorithm.begin(), algorithm.size()));
  JSG_REQUIRE(md != nullptr, Error, kj::str("Unknown digest: ", algorithm));

  auto mdctx = ncrypto::EVPMDCtxPointer::New();
  JSG_REQUIRE(mdctx, Error, "Failed to create verification context");
  JSG_REQUIRE(mdctx.digestInit(md), Error, "Failed to initialize verification context");

  return jsg::alloc<VerifyHandle>(kj::mv(mdctx));
}

void CryptoImpl::VerifyHandle::update(jsg::Lock& js, jsg::BufferSource data) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  JSG_REQUIRE(ctx, Error, "Verification context has already been finalized");
  auto ptr = data.asArrayPtr();
  ncrypto::Buffer<const void> buf{
    .data = ptr.begin(),
    .len = ptr.size(),
  };
  JSG_REQUIRE(ctx.digestUpdate(buf), Error, "Failed to update verification context");
}

bool CryptoImpl::VerifyHandle::verify(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource signature,
    jsg::Optional<int> rsaPadding,
    jsg::Optional<int> maybeSaltLen,
    jsg::Optional<int> dsaSigEnc) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;

  JSG_REQUIRE(ctx, Error, "Verification context has already been finalized");

  const auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "Invalid key for verify operation");

  JSG_REQUIRE(!pkey.isOneShotVariant(), Error, "Unsupported operation for this key");

  auto clonedSignature = signature.clone(js);
  KJ_IF_SOME(enc, dsaSigEnc) {
    static constexpr unsigned kP1363 = 1;
    JSG_REQUIRE(enc <= kP1363 && enc >= 0, Error, "Invalid DSA signature encoding");
    if (enc == kP1363) {
      clonedSignature =
          jsg::BufferSource(js, convertSignatureToDER(js, pkey, clonedSignature.detach(js)));
    }
  }

  return verifyFinal(js, ncrypto::EVPMDCtxPointer(ctx.release()), pkey, clonedSignature,
      rsaPadding.orDefault(pkey.getDefaultSignPadding()), maybeSaltLen);
}

jsg::BufferSource CryptoImpl::signOneShot(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<kj::String> algorithm,
    jsg::BufferSource data,
    jsg::Optional<int> rsaPadding,
    jsg::Optional<int> pssSaltLength,
    jsg::Optional<int> dsaSigEnc) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;

  auto mdctx = ncrypto::EVPMDCtxPointer::New();
  JSG_REQUIRE(mdctx, Error, "Failed to create signing context");

  const auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "Invalid key for sign operation");

  // The version of BoringSSL we use does not support DSA keys with EVP
  // When signing/verification. This may change in the future.
  JSG_REQUIRE(pkey.id() != EVP_PKEY_DSA, Error, "Signing with DSA keys is not currently supported");
  // TODO(later): When DSA keys are supported, uncomment to validate DSA params.
  // JSG_REQUIRE(pkey.validateDsaParameters(), Error, "Invalid DSA parameters");

  auto md = maybeGetDigest(algorithm);

  JSG_REQUIRE(mdctx.signInit(pkey, md).has_value(), Error, "Failed to initialize signing context");

  ncrypto::Buffer<const kj::byte> buf{
    .data = data.asArrayPtr().begin(),
    .len = data.size(),
  };

  ncrypto::DataPointer sig = mdctx.signOneShot(buf);
  kj::ArrayPtr<kj::byte> sigPtr(sig.get<kj::byte>(), sig.size());
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, sig.size());
  backing.asArrayPtr().copyFrom(sigPtr);

  KJ_IF_SOME(enc, dsaSigEnc) {
    static constexpr unsigned kP1363 = 1;
    JSG_REQUIRE(enc <= kP1363 && enc >= 0, Error, "Invalid DSA signature encoding");
    if (enc == kP1363) {
      backing = convertSignatureToP1363(js, pkey, kj::mv(backing));
    }
  }

  return jsg::BufferSource(js, kj::mv(backing));
}

bool CryptoImpl::verifyOneShot(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::Optional<kj::String> algorithm,
    jsg::BufferSource data,
    jsg::BufferSource signature,
    jsg::Optional<int> rsaPadding,
    jsg::Optional<int> pssSaltLength,
    jsg::Optional<int> dsaSigEnc) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;

  auto mdctx = ncrypto::EVPMDCtxPointer::New();
  JSG_REQUIRE(mdctx, Error, "Failed to create verification context");

  const auto& pkey =
      JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "Invalid key for verification operation");

  // The version of BoringSSL we use does not support DSA keys with EVP
  // When signing/verification. This may change in the future.
  JSG_REQUIRE(
      pkey.id() != EVP_PKEY_DSA, Error, "Verifying with DSA keys is not currently supported");
  // TODO(later): When DSA keys are supported, uncomment to validate DSA params.
  // JSG_REQUIRE(pkey.validateDsaParameters(), Error, "Invalid DSA parameters");

  auto md = maybeGetDigest(algorithm);

  JSG_REQUIRE(
      mdctx.verifyInit(pkey, md).has_value(), Error, "Failed to initialize verification context");

  auto clonedSignature = signature.clone(js);
  KJ_IF_SOME(enc, dsaSigEnc) {
    static constexpr unsigned kP1363 = 1;
    JSG_REQUIRE(enc <= kP1363 && enc >= 0, Error, "Invalid DSA signature encoding");
    if (enc == kP1363) {
      clonedSignature =
          jsg::BufferSource(js, convertSignatureToDER(js, pkey, clonedSignature.detach(js)));
    }
  }

  ncrypto::Buffer<const kj::byte> buf{
    .data = data.asArrayPtr().begin(),
    .len = data.size(),
  };

  ncrypto::Buffer<const kj::byte> sig{
    .data = clonedSignature.asArrayPtr().begin(),
    .len = clonedSignature.size(),
  };

  return mdctx.verify(buf, sig);
}

#pragma endregion  // SignVerify

// ======================================================================================
#pragma region Cipher/Decipher

namespace {
constexpr unsigned kNoAuthTagLength = static_cast<unsigned>(-1);

CryptoImpl::CipherHandle::AuthenticatedInfo initAuthenticated(ncrypto::CipherCtxPointer& ctx,
    bool encrypt,
    kj::StringPtr cipher_type,
    int iv_len,
    unsigned int auth_tag_len) {
  ncrypto::MarkPopErrorOnReturn mark_pop_error_on_return;

  JSG_REQUIRE(ctx.setIvLength(iv_len), Error, "Invalid initialization vector");

  CryptoImpl::CipherHandle::AuthenticatedInfo info;
  info.auth_tag_len = auth_tag_len;

  const int mode = ctx.getMode();
  if (mode == EVP_CIPH_GCM_MODE) {
    if (info.auth_tag_len != kNoAuthTagLength) {
      JSG_REQUIRE(ncrypto::Cipher::IsValidGCMTagLength(auth_tag_len), Error,
          "Invalid authentication tag length");
    }
  } else {
    if (auth_tag_len == kNoAuthTagLength) {
      // We treat ChaCha20-Poly1305 specially. Like GCM, the authentication tag
      // length defaults to 16 bytes when encrypting. Unlike GCM, the
      // authentication tag length also defaults to 16 bytes when decrypting,
      // whereas GCM would accept any valid authentication tag length.
      if (ctx.getNid() == NID_chacha20_poly1305) {
        info.auth_tag_len = 16;
      } else {
        JSG_FAIL_REQUIRE(
            Error, kj::str("The auth tag length is required for cipher ", cipher_type));
      }
    }

    if (mode == EVP_CIPH_CCM_MODE && !encrypt && FIPS_mode()) {
      JSG_FAIL_REQUIRE(Error, "CCM encryption not supported in FIPS mode");
    }

    JSG_REQUIRE(
        ctx.setAeadTagLength(info.auth_tag_len), Error, "Invalid authentication tag length");

    if (mode == EVP_CIPH_CCM_MODE) {
      JSG_REQUIRE(iv_len >= 7 && iv_len <= 13, Error, "Invalid authentication tag length");
      if (iv_len == 12) info.max_message_size = 16777215;
      if (iv_len == 13) info.max_message_size = 65535;
    }
  }

  return info;
}

bool isAuthenticatedMode(const ncrypto::CipherCtxPointer& ctx) {
  return ncrypto::Cipher::FromCtx(ctx).isSupportedAuthenticatedMode();
}

bool passAuthTagToOpenSSL(ncrypto::CipherCtxPointer& ctx, kj::ArrayPtr<const kj::byte> authTag) {
  ncrypto::Buffer<const char> buffer{
    .data = reinterpret_cast<const char*>(authTag.begin()),
    .len = authTag.size(),
  };
  return ctx.setAeadTag(buffer);
}
}  // namespace

CryptoImpl::CipherHandle::CipherHandle(Mode mode,
    ncrypto::CipherCtxPointer ctx,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource iv,
    kj::Maybe<AuthenticatedInfo> maybeAuthInfo)
    : mode(mode),
      ctx(kj::mv(ctx)),
      key(kj::mv(key)),
      iv(kj::mv(iv)),
      maybeAuthInfo(kj::mv(maybeAuthInfo)) {}

jsg::Ref<CryptoImpl::CipherHandle> CryptoImpl::CipherHandle::constructor(jsg::Lock& js,
    kj::String mode,
    kj::String algorithm,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource iv,
    jsg::Optional<uint32_t> maybeAuthTagLength) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  JSG_REQUIRE(key->getType() == "secret"_kj, TypeError, "Invalid key type for cipher");

  std::string_view name(algorithm.begin(), algorithm.size());
  auto cipher = ncrypto::Cipher::FromName(name);
  JSG_REQUIRE(cipher, Error, kj::str("Unknown or unsupported cipher: ", algorithm));

  auto keyData =
      JSG_REQUIRE_NONNULL(tryGetSecretKeyData(key), Error, "Failed to get raw secret key data");

  int expectedIvLength = cipher.getIvLength();

  if ((expectedIvLength && !iv.size()) ||
      (!cipher.isSupportedAuthenticatedMode() && iv.size() &&
          static_cast<int>(iv.size()) != expectedIvLength)) {
    JSG_FAIL_REQUIRE(Error, "Invalid initialization vector");
  }

  if (cipher.getNid() == NID_chacha20_poly1305) {
    JSG_REQUIRE(iv.size(), Error, "ChaCha20-Polcy1305 requires an initialization vector");
    JSG_REQUIRE(iv.size() <= 12, Error, "Invalid initialization vector");
  }

  auto ctx = ncrypto::CipherCtxPointer::New();
  JSG_REQUIRE(ctx, Error, "Failed to create cipher/decipher context");

  if (cipher.getMode() == EVP_CIPH_WRAP_MODE) {
    ctx.setFlags(EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  }

  bool encrypt = mode == "cipher"_kj;

  JSG_REQUIRE(ctx.init(cipher, encrypt), Error, "Failed to initialize cipher/decipher context");

  kj::Maybe<CryptoImpl::CipherHandle::AuthenticatedInfo> maybeAuthInfo = kj::none;
  if (cipher.isSupportedAuthenticatedMode()) {
    maybeAuthInfo = initAuthenticated(
        ctx, encrypt, algorithm, iv.size(), maybeAuthTagLength.orDefault(kNoAuthTagLength));
  }

  JSG_REQUIRE(ctx.setKeyLength(keyData.size()), Error, "Invalid key length");

  JSG_REQUIRE(ctx.init(ncrypto::Cipher(), encrypt, keyData.begin(), iv.asArrayPtr().begin()), Error,
      "Failed to initialize cipher/cipher context");

  return jsg::alloc<CipherHandle>(mode == "cipher" ? Mode::CIPHER : Mode::DECIPHER, kj::mv(ctx),
      kj::mv(key), kj::mv(iv), kj::mv(maybeAuthInfo));
}

jsg::BufferSource CryptoImpl::CipherHandle::update(jsg::Lock& js, jsg::BufferSource data) {

  JSG_REQUIRE(ctx, Error, "Cipher/decipher context has already been finalized");
  JSG_REQUIRE(data.size() <= INT_MAX, Error, "Data too large");

  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  const int ctxMode = ctx.getMode();

  if (ctxMode == EVP_CIPH_CCM_MODE) {
    auto max = KJ_ASSERT_NONNULL(maybeAuthInfo).max_message_size;
    JSG_REQUIRE(data.size() <= max, Error, "Invalid message length");
  }

  if (mode == Mode::DECIPHER && isAuthenticatedMode(ctx) && !authTagPassed) {
    authTagPassed = true;
    auto& tag = JSG_REQUIRE_NONNULL(maybeAuthTag, Error, "No auth tag provided");
    JSG_REQUIRE(
        passAuthTagToOpenSSL(ctx, tag.asArrayPtr().asConst()), Error, "Failed to set auth tag");
  }

  const int block_size = ctx.getBlockSize();
  KJ_ASSERT(block_size > 0);
  JSG_REQUIRE(data.size() + block_size <= INT_MAX, Error, "Data too large");
  int buf_len = data.size() + block_size;

  ncrypto::Buffer<const unsigned char> buffer = {
    .data = data.asArrayPtr().begin(),
    .len = data.size(),
  };
  if (mode == Mode::CIPHER && ctxMode == EVP_CIPH_WRAP_MODE &&
      !ctx.update(buffer, nullptr, &buf_len)) {
    JSG_FAIL_REQUIRE(Error, "Failed to process data");
  }

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, buf_len);
  buffer.data = data.asArrayPtr().begin();
  buffer.len = data.size();
  bool r = ctx.update(buffer, backing.asArrayPtr().begin(), &buf_len);

  if (buf_len != backing.size()) {
    JSG_REQUIRE(buf_len < backing.size(), Error, "Invalid buffer length");
    auto newBacking = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, buf_len);
    if (buf_len > 0) {
      newBacking.asArrayPtr().copyFrom(backing.asArrayPtr().slice(0, buf_len));
    }
    backing = kj::mv(newBacking);
  }

  // When in CCM mode, EVP_CipherUpdate will fail if the authentication tag is
  // invalid. In that case, remember the error and throw in final().
  if (!r && mode == Mode::DECIPHER && ctxMode == EVP_CIPH_CCM_MODE) {
    pendingAuthFailed = true;
  }

  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource CryptoImpl::CipherHandle::final(jsg::Lock& js) {
  JSG_REQUIRE(ctx, Error, "Cipher/decipher context has already been finalized");

  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  int ctxMode = ctx.getMode();

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, ctx.getBlockSize());

  if (mode == Mode::DECIPHER && isAuthenticatedMode(ctx) && !authTagPassed) {
    authTagPassed = true;
    auto& tag = JSG_REQUIRE_NONNULL(maybeAuthTag, Error, "No auth tag provided");
    JSG_REQUIRE(
        passAuthTagToOpenSSL(ctx, tag.asArrayPtr().asConst()), Error, "Failed to set auth tag");
  }

  if (ctx.getNid() == NID_chacha20_poly1305 && mode == Mode::DECIPHER) {
    JSG_REQUIRE(authTagPassed, Error, "An auth tag is required");
  }

  // In CCM mode, final() only checks whether authentication failed in update().
  // EVP_CipherFinal_ex must not be called and will fail.
  bool ok;
  if (mode == Mode::DECIPHER && ctxMode == EVP_CIPH_CCM_MODE) {
    ok = !pendingAuthFailed;
    backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
  } else {
    int out_len = backing.size();
    ok = ctx.update({}, backing.asArrayPtr().begin(), &out_len, true);

    if (out_len != backing.size()) {
      JSG_REQUIRE(out_len < backing.size(), Error, "Invalid buffer length");
      auto newBacking = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, out_len);
      if (out_len > 0) {
        newBacking.asArrayPtr().copyFrom(backing.asArrayPtr().slice(0, out_len));
      }
      backing = kj::mv(newBacking);
    }

    if (ok && mode == Mode::CIPHER && isAuthenticatedMode(ctx)) {
      auto& info = JSG_REQUIRE_NONNULL(maybeAuthInfo, Error, "Missing required auth info");
      // In GCM mode, the authentication tag length can be specified in advance,
      // but defaults to 16 bytes when encrypting. In CCM and OCB mode, it must
      // always be given by the user.
      if (info.auth_tag_len == kNoAuthTagLength) {
        info.auth_tag_len = 16;
      }
      auto authTagBacking = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, info.auth_tag_len);
      ok = ctx.getAeadTag(info.auth_tag_len, authTagBacking.asArrayPtr().begin());
      maybeAuthTag = jsg::BufferSource(js, kj::mv(authTagBacking));
    }
  }

  JSG_REQUIRE(ok, Error, "Authentication failed");

  ctx.reset();
  return jsg::BufferSource(js, kj::mv(backing));
}

void CryptoImpl::CipherHandle::setAAD(
    jsg::Lock& js, jsg::BufferSource aad, jsg::Optional<uint32_t> maybePlaintextLength) {

  JSG_REQUIRE(ctx, Error, "Cipher/decipher context has already been finalized");
  JSG_REQUIRE(isAuthenticatedMode(ctx), Error, "Cipher does not support authenticated mode");

  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  int outlen;
  const int ctxMode = ctx.getMode();

  // When in CCM mode, we need to set the authentication tag and the plaintext
  // length in advance.
  if (ctxMode == EVP_CIPH_CCM_MODE) {
    auto plaintextLength = JSG_REQUIRE_NONNULL(
        maybePlaintextLength, Error, "options.plaintextLength is required for CCM mode with AAD");

    auto& info = JSG_REQUIRE_NONNULL(maybeAuthInfo, Error, "Required auth info is not available");

    JSG_REQUIRE(plaintextLength <= info.max_message_size, Error, "Data too large");

    if (mode == Mode::DECIPHER && isAuthenticatedMode(ctx) && !authTagPassed) {
      authTagPassed = true;
      auto& tag = JSG_REQUIRE_NONNULL(maybeAuthTag, Error, "No auth tag provided");
      JSG_REQUIRE(
          passAuthTagToOpenSSL(ctx, tag.asArrayPtr().asConst()), Error, "Failed to set auth tag");
    }

    ncrypto::Buffer<const unsigned char> buffer{
      .data = nullptr,
      .len = plaintextLength,
    };
    // Specify the plaintext length.
    JSG_REQUIRE(ctx.update(buffer, nullptr, &outlen), Error, "Failed to set plaintext length");
  }

  ncrypto::Buffer<const unsigned char> buffer{
    .data = aad.asArrayPtr().begin(),
    .len = aad.size(),
  };
  JSG_REQUIRE(ctx.update(buffer, nullptr, &outlen), Error, "Failed to set AAD");
}

void CryptoImpl::CipherHandle::setAutoPadding(jsg::Lock& js, bool autoPadding) {
  JSG_REQUIRE(ctx, Error, "Cipher/decipher context has already been finalized");
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;
  JSG_REQUIRE(ctx.setPadding(autoPadding), Error, "Failed to set autopadding");
}

void CryptoImpl::CipherHandle::setAuthTag(jsg::Lock& js, jsg::BufferSource authTag) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;
  JSG_REQUIRE(ctx, Error, "Cipher/decipher context has already been finalized");
  JSG_REQUIRE(isAuthenticatedMode(ctx), Error, "Cipher does not support authenticated mode");
  JSG_REQUIRE(mode == Mode::DECIPHER, Error, "Setting auth tag only support in decipher mode");
  JSG_REQUIRE(maybeAuthTag == kj::none, Error, "Auth tag is already set");
  JSG_REQUIRE(authTag.size() <= INT_MAX, Error, "Auth tag is too big");

  int ctxMode = ctx.getMode();
  bool is_valid = false;

  auto& info = JSG_REQUIRE_NONNULL(maybeAuthInfo, Error, "Required auth info is not available");

  if (ctxMode == EVP_CIPH_GCM_MODE) {
    // Restrict GCM tag lengths according to NIST 800-38d, page 9.
    is_valid = (info.auth_tag_len == kNoAuthTagLength || info.auth_tag_len == authTag.size()) &&
        ncrypto::Cipher::IsValidGCMTagLength(authTag.size());
  } else {
    is_valid = info.auth_tag_len == authTag.size();
  }

  JSG_REQUIRE(is_valid, Error, "Invalid authentication tag length");

  info.auth_tag_len = authTag.size();

  // We defensively copy the auth tag here to prevent modification.
  maybeAuthTag = authTag.copy(js);
}

jsg::BufferSource CryptoImpl::CipherHandle::getAuthTag(jsg::Lock& js) {
  JSG_REQUIRE(!ctx, Error, "Auth tag is only available once cipher context has been finalized");
  JSG_REQUIRE(mode == Mode::CIPHER, Error, "Getting the auth tag is only support for cipher");

  KJ_IF_SOME(tag, maybeAuthTag) {
    KJ_DEFER(maybeAuthTag = kj::none);
    return kj::mv(tag);
  }

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
  return jsg::BufferSource(js, kj::mv(backing));
}

namespace {

// TODO(soon): For some reason the ncrypto implementation of these is not
// working for us but they do work in Node.js. Will need to figure out why.
// For now, it's easy enough to implement ourselves here.
using EVP_PKEY_cipher_t = int(
    EVP_PKEY_CTX* ctx, unsigned char* out, size_t* outlen, const unsigned char* in, size_t inlen);

template <EVP_PKEY_cipher_t cipher>
jsg::BufferSource Cipher(jsg::Lock& js,
    ncrypto::EVPKeyCtxPointer&& ctx,
    const jsg::BufferSource& buffer,
    const CryptoImpl::PublicPrivateCipherOptions& options) {

  ncrypto::ClearErrorOnReturn clearErrorOnReturn;

  const EVP_MD* digest = nullptr;
  if (options.oaepHash.size() > 0) {
    std::string_view name(options.oaepHash.begin(), options.oaepHash.size());
    digest = ncrypto::getDigestByName(name);
    JSG_REQUIRE(digest != nullptr, Error, "Unsupported hash digest");
  }

  JSG_REQUIRE(
      EVP_PKEY_CTX_set_rsa_padding(ctx.get(), options.padding), Error, "Failed to set the padding");

  if (digest != nullptr && options.padding == RSA_PKCS1_OAEP_PADDING) {
    JSG_REQUIRE(
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), digest) == 1, Error, "Failed to set the digest");
    JSG_REQUIRE(EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), digest) == 1, Error,
        "Failed to set the mgf1 digest");
  }

  KJ_IF_SOME(label, options.oaepLabel) {
    // The ctx takes ownership of the data buffer so we have to copy.
    auto data = ncrypto::DataPointer::Alloc(label.size());
    kj::ArrayPtr<kj::byte> dataPtr(data.get<kj::byte>(), data.size());
    dataPtr.copyFrom(label.asArrayPtr());
    auto released = data.release();
    JSG_REQUIRE(EVP_PKEY_CTX_set0_rsa_oaep_label(
                    ctx.get(), static_cast<uint8_t*>(released.data), released.len) == 1,
        Error, "Failed to set the OAEP label");
  }

  size_t len;
  JSG_REQUIRE(cipher(ctx.get(), nullptr, &len, buffer.asArrayPtr().begin(), buffer.size()) == 1,
      Error, "Failed to determine output size");

  if (len == 0) {
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
    return jsg::BufferSource(js, kj::mv(backing));
  }

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, len);
  JSG_REQUIRE(cipher(ctx.get(), backing.asArrayPtr().begin(), &len, buffer.asArrayPtr().begin(),
                  buffer.size()) == 1,
      Error, "Failed to cipher/decipher");

  if (len < backing.size()) {
    auto newBacking = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, len);
    newBacking.asArrayPtr().copyFrom(backing.asArrayPtr().slice(0, len));
    backing = kj::mv(newBacking);
  }

  return jsg::BufferSource(js, kj::mv(backing));
}
}  // namespace

jsg::BufferSource CryptoImpl::publicEncrypt(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource buffer,
    CryptoImpl::PublicPrivateCipherOptions options) {
  auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "No key provided");
  JSG_REQUIRE(pkey.isRsaVariant(), Error, "publicEncrypt() currently only supports RSA keys");
  auto ctx = pkey.newCtx();
  JSG_REQUIRE(ctx.initForEncrypt(), Error, "Failed to init for encryption");
  return Cipher<EVP_PKEY_encrypt>(js, kj::mv(ctx), buffer, options);
}

jsg::BufferSource CryptoImpl::privateDecrypt(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource buffer,
    CryptoImpl::PublicPrivateCipherOptions options) {
  auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "No key provided");
  JSG_REQUIRE(pkey.isRsaVariant(), Error, "publicEncrypt() currently only supports RSA keys");
  auto ctx = pkey.newCtx();
  JSG_REQUIRE(ctx.initForDecrypt(), Error, "Failed to init for decryption");
  return Cipher<EVP_PKEY_decrypt>(js, kj::mv(ctx), buffer, options);
}

jsg::BufferSource CryptoImpl::publicDecrypt(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource buffer,
    CryptoImpl::PublicPrivateCipherOptions options) {
  auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "No key provided");
  JSG_REQUIRE(pkey.isRsaVariant(), Error, "publicEncrypt() currently only supports RSA keys");
  auto ctx = pkey.newCtx();
  JSG_REQUIRE(EVP_PKEY_verify_recover_init(ctx.get()) == 1, Error, "Failed to init for decryption");
  return Cipher<EVP_PKEY_verify_recover>(js, kj::mv(ctx), buffer,
      {
        .padding = options.padding,
        .oaepHash = kj::String(),
      });
}
jsg::BufferSource CryptoImpl::privateEncrypt(jsg::Lock& js,
    jsg::Ref<CryptoKey> key,
    jsg::BufferSource buffer,
    CryptoImpl::PublicPrivateCipherOptions options) {
  auto& pkey = JSG_REQUIRE_NONNULL(tryGetKey(key), Error, "No key provided");
  JSG_REQUIRE(pkey.isRsaVariant(), Error, "publicEncrypt() currently only supports RSA keys");
  auto ctx = pkey.newCtx();
  JSG_REQUIRE(EVP_PKEY_sign_init(ctx.get()) == 1, Error, "Failed to init for encryption");
  return Cipher<EVP_PKEY_sign>(js, kj::mv(ctx), buffer,
      {
        .padding = options.padding,
        .oaepHash = kj::String(),
      });
}

namespace {
ncrypto::Cipher getCipher(kj::OneOf<kj::String, int>& nameOrNid) {
  KJ_SWITCH_ONEOF(nameOrNid) {
    KJ_CASE_ONEOF(nid, int) {
      return ncrypto::Cipher::FromNid(nid);
    }
    KJ_CASE_ONEOF(name, kj::String) {
      std::string_view nameStr(name.cStr(), name.size());
      return ncrypto::Cipher::FromName(nameStr);
    }
  }
  return {};
}
}  // namespace

jsg::Optional<CryptoImpl::CipherInfo> CryptoImpl::getCipherInfo(
    kj::OneOf<kj::String, int> nameOrNid, CryptoImpl::GetCipherInfoOptions options) {

  if (auto cipher = getCipher(nameOrNid)) {

    int keyLength = cipher.getKeyLength();
    int ivLength = cipher.getIvLength();

    if (options.ivLength != kj::none || options.keyLength != kj::none) {
      auto ctx = ncrypto::CipherCtxPointer::New();
      if (!ctx.init(cipher, true)) return kj::none;
      KJ_IF_SOME(len, options.keyLength) {
        if (!ctx.setKeyLength(len)) return kj::none;
        keyLength = len;
      }
      KJ_IF_SOME(len, options.ivLength) {
        // For CCM modes, the IV may be between 7 and 13 bytes.
        // For GCM and OCB modes, we'll check by attempting to
        // set the value. For everything else, just check that
        // check_len == iv_length.
        switch (cipher.getMode()) {
          case EVP_CIPH_CCM_MODE: {
            if (len < 7 || len > 13) return kj::none;
            break;
          }
          case EVP_CIPH_GCM_MODE: {
            if (!ctx.setIvLength(len)) return kj::none;
            break;
          }
          case EVP_CIPH_OCB_MODE: {
            if (!ctx.setIvLength(len)) return kj::none;
            break;
          }
          default:
            if (len != ivLength) return kj::none;
            break;
        }
        ivLength = len;
      }
    }

    auto nameView = cipher.getName();
    auto modeView = cipher.getModeLabel();
    kj::String name = kj::str(kj::heapArray<char>(nameView.data(), nameView.size()));
    kj::String mode = kj::str(kj::heapArray<char>(modeView.data(), modeView.size()));
    return CipherInfo{
      .name = kj::mv(name),
      .nid = cipher.getNid(),
      .blockSize = cipher.getBlockSize(),
      .ivLength = ivLength,
      .keyLength = keyLength,
      .mode = kj::mv(mode),
    };
  }

  return kj::none;
}

#pragma endregion  // Cipher/Decipher

// =============================================================================
#pragma region ECDH

namespace {
ncrypto::ECPointPointer bufferToPoint(const EC_GROUP* group, jsg::BufferSource& buf) {
  JSG_REQUIRE(buf.size() <= INT32_MAX, Error, "buffer is too big");

  auto pub = ncrypto::ECPointPointer::New(group);
  JSG_REQUIRE(pub, Error, "Failed to allocate EC_POINT for a public key");

  ncrypto::Buffer<const unsigned char> buffer{
    .data = buf.asArrayPtr().begin(),
    .len = buf.size(),
  };

  JSG_REQUIRE(pub.setFromBuffer(buffer, group), Error, "Failed to set point");
  return pub;
}

point_conversion_form_t getFormat(kj::StringPtr format) {
  if (format == "compressed"_kj) return POINT_CONVERSION_COMPRESSED;
  if (format == "uncompressed"_kj) return POINT_CONVERSION_UNCOMPRESSED;
  if (format == "hybrid"_kj) return POINT_CONVERSION_HYBRID;
  JSG_FAIL_REQUIRE(Error, "Invalid ECDH public key format");
}

jsg::BufferSource ecPointToBuffer(
    jsg::Lock& js, const EC_GROUP* group, const EC_POINT* point, point_conversion_form_t form) {
  size_t len = EC_POINT_point2oct(group, point, form, nullptr, 0, nullptr);
  JSG_REQUIRE(len != 0, Error, "Failed to get public key length");

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, len);

  len =
      EC_POINT_point2oct(group, point, form, backing.asArrayPtr().begin(), backing.size(), nullptr);
  JSG_REQUIRE(len != 0, Error, "Failed to get public key");

  return jsg::BufferSource(js, kj::mv(backing));
}

bool isKeyValidForCurve(const EC_GROUP* group, const ncrypto::BignumPointer& private_key) {
  // Private keys must be in the range [1, n-1].
  // Ref: Section 3.2.1 - http://www.secg.org/sec1-v2.pdf
  if (private_key < ncrypto::BignumPointer::One()) {
    return false;
  }
  auto order = ncrypto::BignumPointer::New();
  JSG_REQUIRE(order, Error, "Internal failure when checking ECDH key");
  return EC_GROUP_get_order(group, order.get(), nullptr) && private_key < order;
}
}  // namespace

CryptoImpl::ECDHHandle::ECDHHandle(ncrypto::ECKeyPointer key)
    : key_(kj::mv(key)),
      group_(key_.getGroup()) {}

jsg::Ref<CryptoImpl::ECDHHandle> CryptoImpl::ECDHHandle::constructor(
    jsg::Lock& js, kj::String curveName) {

  int nid = OBJ_sn2nid(curveName.begin());
  JSG_REQUIRE(nid != NID_undef, Error, "Invalid curve");

  auto key = ncrypto::ECKeyPointer::NewByCurveName(nid);
  JSG_REQUIRE(key, Error, "Failed to create key using named curve");

  return jsg::alloc<CryptoImpl::ECDHHandle>(kj::mv(key));
}

jsg::BufferSource CryptoImpl::ECDHHandle::computeSecret(
    jsg::Lock& js, jsg::BufferSource otherPublicKey) {

  ncrypto::ClearErrorOnReturn clear_error_on_return;

  JSG_REQUIRE(key_.checkKey(), Error, "Invalid keypair");

  auto pub = bufferToPoint(group_, otherPublicKey);
  JSG_REQUIRE(pub, Error, "Invalid to set ECDH public key");

  int field_size = EC_GROUP_get_degree(group_);
  size_t out_len = (field_size + 7) / 8;

  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, out_len);

  JSG_REQUIRE(ECDH_compute_key(backing.asArrayPtr().begin(), out_len, pub, key_.get(), nullptr),
      Error, "Failed to compute ECDH key");

  return jsg::BufferSource(js, kj::mv(backing));
}

void CryptoImpl::ECDHHandle::generateKeys() {
  ncrypto::ClearErrorOnReturn clear_error_on_return;
  JSG_REQUIRE(key_.generate(), Error, "Failed to generate keys");
}

jsg::BufferSource CryptoImpl::ECDHHandle::getPrivateKey(jsg::Lock& js) {
  auto b = key_.getPrivateKey();
  JSG_REQUIRE(b != nullptr, Error, "Failed to get ECDH private key");
  auto backing =
      jsg::BackingStore::alloc<v8::ArrayBuffer>(js, ncrypto::BignumPointer::GetByteCount(b));
  JSG_REQUIRE(backing.size() ==
          ncrypto::BignumPointer::EncodePaddedInto(b, backing.asArrayPtr().begin(), backing.size()),
      Error, "Failed to encode the private key");
  return jsg::BufferSource(js, kj::mv(backing));
}

jsg::BufferSource CryptoImpl::ECDHHandle::getPublicKey(jsg::Lock& js, kj::String format) {
  const auto group = key_.getGroup();
  const auto pub = key_.getPublicKey();
  JSG_REQUIRE(pub != nullptr, Error, "Failed to get ECDH public key");
  point_conversion_form_t form = getFormat(format);
  return ecPointToBuffer(js, group, pub, form);
}

void CryptoImpl::ECDHHandle::setPrivateKey(jsg::Lock& js, jsg::BufferSource key) {

  JSG_REQUIRE(key.size() <= INT32_MAX, Error, "key is too big");

  ncrypto::BignumPointer priv(key.asArrayPtr().begin(), key.size());
  JSG_REQUIRE(priv, Error, "Failed to convert buffer to BN");

  JSG_REQUIRE(
      isKeyValidForCurve(group_, priv), Error, "Private key is not valid for specified curve.");

  auto new_key = key_.clone();
  JSG_REQUIRE(new_key, Error, "Internal error when setting private key");

  bool result = new_key.setPrivateKey(priv);
  priv.reset();

  JSG_REQUIRE(result, Error, "Failed to convert BN to a private key");

  ncrypto::ClearErrorOnReturn clear_error_on_return;

  auto priv_key = new_key.getPrivateKey();
  JSG_REQUIRE(priv_key, Error, "Failed to get ECDH private key");

  auto pub = ncrypto::ECPointPointer::New(group_);
  JSG_REQUIRE(pub, Error, "Internal error when initializing new EC point");

  JSG_REQUIRE(pub.mul(group_, priv_key), Error, "Failed to generate ECDH public key");

  JSG_REQUIRE(new_key.setPublicKey(pub), Error, "Failed to set generated public key");

  key_ = std::move(new_key);
  group_ = key_.getGroup();
}

jsg::BufferSource CryptoImpl::ECDHHandle::convertKey(
    jsg::Lock& js, jsg::BufferSource key, kj::String curveName, kj::String format) {
  ncrypto::ClearErrorOnReturn clear_error_on_return;

  JSG_REQUIRE(key.size() <= INT32_MAX, Error, "key is too big");
  if (key.size() == 0) {
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
    return jsg::BufferSource(js, kj::mv(backing));
  }

  int nid = OBJ_sn2nid(curveName.begin());
  JSG_REQUIRE(nid != NID_undef, Error, "Invalid curve");

  auto group = ncrypto::ECGroupPointer::NewByCurveName(nid);

  auto pub = bufferToPoint(group, key);
  JSG_REQUIRE(pub, Error, "Failed to convert buffer to EC_POINT");

  point_conversion_form_t form = getFormat(format);

  return ecPointToBuffer(js, group, pub, form);
}

#pragma endregion  // ECDH

}  // namespace workerd::api::node
