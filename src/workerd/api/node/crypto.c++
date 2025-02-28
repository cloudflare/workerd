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

}  // namespace workerd::api::node
