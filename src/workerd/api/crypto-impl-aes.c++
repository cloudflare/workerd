// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "crypto-impl.h"
#include <algorithm>
#include <cstdint>
#include <openssl/aes.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <workerd/io/io-context.h>

namespace workerd::api {
namespace {

std::unique_ptr<EVP_CIPHER_CTX, void(*)(EVP_CIPHER_CTX*)> makeCipherContext() {
  return {EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
}

using UniqueBignum = std::unique_ptr<BIGNUM, void(*)(BIGNUM*)>;

UniqueBignum newBignum() {
  return {BN_new(), BN_free};
}

auto lookupAesCbcType(uint bitLength) {
  switch (bitLength) {
    case 128: return EVP_aes_128_cbc();
    case 192: return EVP_aes_192_cbc();
    case 256: return EVP_aes_256_cbc();
    default: KJ_FAIL_ASSERT("CryptoKey has invalid data length", bitLength);
    // Assert because the data length must have come from a key we created!
  }
}

auto lookupAesGcmType(uint bitLength) {
  switch (bitLength) {
    case 128: return EVP_aes_128_gcm();
    case 192: return EVP_aes_192_gcm();
    case 256: return EVP_aes_256_gcm();
    default: KJ_FAIL_ASSERT("CryptoKey has invalid data length", bitLength);
    // Assert because the data length must have come from a key we created!
  }
}

void validateAesGcmTagLength(int tagLength) {
  // Ensure the tagLength passed to the AES-GCM algorithm is one of the allowed bit lengths.

  switch (tagLength) {
    case 32:
    case 64:
    case 96:
    case 104:
    case 112:
    case 120:
    case 128: break;
    default:
      JSG_FAIL_REQUIRE(DOMOperationError, "Invalid AES-GCM tag length ", tagLength, ".");
  }
}

int decryptFinalHelper(kj::StringPtr algorithm, size_t inputLength,
    size_t outputLength, EVP_CIPHER_CTX* cipherCtx, kj::byte* out) {
  // EVP_DecryptFinal_ex() failures can mean a mundane decryption failure, so we have to be careful
  // with error handling when calling it. We can't use our usual OSSLCALL() macro, because that
  // throws an unhelpful opaque OperationError.

  // Clear the error queue; who knows what kind of junk is in there.
  ERR_clear_error();

  int finalPlainSize = 0;
  if (EVP_DecryptFinal_ex(cipherCtx, out, &finalPlainSize)) {
    return finalPlainSize;
  }

  // Decryption failure! Let's figure out what exception to throw.

  auto ec = ERR_peek_error();

  // If the error code is anything other than zero or BAD_DECRYPT, just throw an opaque
  // OperationError for consistency with our OSSLCALL() macro. Notably, AES-GCM tag authentication
  // failures don't produce any error code, though they should probably be BAD_DECRYPT.
  JSG_REQUIRE(ec == 0 || ec == ERR_PACK(ERR_LIB_CIPHER, CIPHER_R_BAD_DECRYPT) ||
      ec == ERR_PACK(ERR_LIB_CIPHER, CIPHER_R_WRONG_FINAL_BLOCK_LENGTH), InternalDOMOperationError,
      "Unexpected issue decrypting", internalDescribeOpensslErrors());

  // Consume the error since it's one we were expecting.
  ERR_get_error();

  // Otherwise, tell the script author they gave us garbage.
  JSG_FAIL_REQUIRE(DOMOperationError, "Decryption failed. This could be due "
      "to a ciphertext authentication failure, bad padding, incorrect CryptoKey, or another "
      "algorithm-specific reason. Input length was ", inputLength,", output length expected to be ",
      outputLength, " for ", algorithm);
}

// NOTE: The OpenSSL calls to implement AES-GCM and AES-CBC are quite similar. If you update one
//   algorithm's encrypt() or decrypt() implementation, it'd be worth reviewing the other
//   algorithm's implementation as well.

class AesKeyBase: public CryptoKey::Impl {
  // The base key is used to avoid repeating the JWK export logic. It also happens to simplify the
  // concrete implementations to only define encrypt/decrypt.

public:
  explicit AesKeyBase(kj::Array<kj::byte> keyData, CryptoKey::AesKeyAlgorithm keyAlgorithm,
                      bool extractable, CryptoKeyUsageSet usages)
      : CryptoKey::Impl(extractable, usages),
        keyData(kj::mv(keyData)), keyAlgorithm(kj::mv(keyAlgorithm)) {}

protected:
  kj::StringPtr getAlgorithmName() const override final {
    // AesKeyAlgorithm is constructed from normalizedName which points into the static constant
    // defined in crypto.c++ for lookup.
    return keyAlgorithm.name;
  }

private:
  CryptoKey::AlgorithmVariant getAlgorithm() const override final { return keyAlgorithm; }

  SubtleCrypto::ExportKeyData exportKey(kj::StringPtr format) const override final {
    JSG_REQUIRE(format == "raw" || format == "jwk", DOMNotSupportedError,
        getAlgorithmName(), " key only supports exporting \"raw\" & \"jwk\", not \"", format,
        "\".");

    if (format == "jwk") {
      auto lengthInBytes = keyData.size();
      KJ_ASSERT(lengthInBytes == 16 || lengthInBytes == 24 || lengthInBytes == 32);

      auto aesMode = keyAlgorithm.name.slice(4);

#ifdef KJ_DEBUG
      static constexpr auto expectedModes = {"GCM", "KW", "CTR", "CBC"};
      KJ_DASSERT(expectedModes.end() != std::find(
          expectedModes.begin(), expectedModes.end(), aesMode));
#endif

      SubtleCrypto::JsonWebKey jwk;
      jwk.kty = kj::str("oct");
      jwk.k = kj::encodeBase64Url(keyData);
      jwk.alg = kj::str("A", lengthInBytes * 8, aesMode);
      jwk.key_ops = getUsages().map([](auto usage) { return kj::str(usage.name()); });
      // I don't know why the spec says:
      //   Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
      // Earlier in the normative part of the spec it says:
      //   6. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
      //   7. Let result be the result of performing the export key operation specified by the
      //      [[algorithm]] internal slot of key using key and format.
      // So there's not really any other value that `ext` can have here since this code is the
      // implementation of step 7 (see SubtleCrypto::exportKey where you can confirm it is
      // enforcing step 6).
      jwk.ext = true;

      return jwk;
    }

    return kj::heapArray(keyData.asPtr());
  }

protected:
  kj::Array<kj::byte> keyData;
  CryptoKey::AesKeyAlgorithm keyAlgorithm;
};

class AesGcmKey final: public AesKeyBase {
public:
  explicit AesGcmKey(kj::Array<kj::byte> keyData, CryptoKey::AesKeyAlgorithm keyAlgorithm,
                     bool extractable, CryptoKeyUsageSet usages)
      : AesKeyBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable, usages) {}

private:
  kj::Array<kj::byte> encrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    kj::ArrayPtr<kj::byte> iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError,
        "Missing field \"iv\" in \"algorithm\".");
    JSG_REQUIRE(iv.size() != 0, DOMOperationError, "AES-GCM IV must not be empty.");

    auto additionalData = algorithm.additionalData.orDefault(kj::Array<kj::byte>()).asPtr();

    // The magic number below came from here:
    // https://w3c.github.io/webcrypto/Overview.html#aes-gcm-operations
    JSG_REQUIRE(plainText.size() <= ((UINT64_C(1) << 39) - 256), DOMOperationError,
        "AES-GCM can only encrypt up to 2^39 - 256 bytes of plaintext at a time, but requested ",
        plainText.size(), " bytes.");

    int tagLength = algorithm.tagLength.orDefault(128);
    validateAesGcmTagLength(tagLength);

    auto cipherCtx = makeCipherContext();
    KJ_ASSERT(cipherCtx.get() != nullptr);

    auto type = lookupAesGcmType(keyData.size() * 8);

    // Set up the cipher context with the initialization vector. We pass nullptrs for the key data
    // and initialization vector because we may need to override the default IV length.
    OSSLCALL(EVP_EncryptInit_ex(cipherCtx.get(), type, nullptr, nullptr, nullptr));
    OSSLCALL(EVP_CIPHER_CTX_ctrl(cipherCtx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                 iv.size(), nullptr));
    OSSLCALL(EVP_EncryptInit_ex(cipherCtx.get(), nullptr, nullptr, keyData.begin(),
                                iv.begin()));

    if (additionalData.size() > 0) {
      // Run the engine with the additional data, which will presumably be transmitted alongside the
      // cipher text in plain text. I noticed that if I call EncryptUpdate with 0-length AAD here,
      // the subsequent call to EncryptUpdate will fail, thus the if-check.
      int dummy;
      OSSLCALL(EVP_EncryptUpdate(cipherCtx.get(), nullptr, &dummy,
                                 additionalData.begin(), additionalData.size()));
    }

    // We make two cipher calls: EVP_EncryptUpdate() and EVP_EncryptFinal_ex(). AES-GCM behaves like
    // a stream cipher in that it does not add padding and can process partial blocks, meaning that
    // we know the exact ciphertext size in advance.
    auto tagByteSize = tagLength / 8;
    auto cipherText = kj::heapArray<kj::byte>(plainText.size() + tagByteSize);

    // Perform the actual encryption.

    int cipherSize = 0;
    OSSLCALL(EVP_EncryptUpdate(cipherCtx.get(), cipherText.begin(), &cipherSize,
                               plainText.begin(), plainText.size()));
    KJ_ASSERT(cipherSize == plainText.size(), "EVP_EncryptUpdate should encrypt all at once");

    int finalCipherSize = 0;
    OSSLCALL(EVP_EncryptFinal_ex(cipherCtx.get(), cipherText.begin() + cipherSize,
                                 &finalCipherSize));
    KJ_ASSERT(finalCipherSize == 0, "EVP_EncryptFinal_ex should not output any data");

    // Concatenate the tag onto the cipher text.
    KJ_ASSERT(cipherSize + tagByteSize == cipherText.size(), "imminent buffer overrun");
    OSSLCALL(EVP_CIPHER_CTX_ctrl(cipherCtx.get(), EVP_CTRL_GCM_GET_TAG,
                                 tagByteSize, cipherText.begin() + cipherSize));
    cipherSize += tagByteSize;
    KJ_ASSERT(cipherSize == cipherText.size(), "buffer overrun");

    return cipherText;
  }

  kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    kj::ArrayPtr<kj::byte> iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError,
        "Missing field \"iv\" in \"algorithm\".");
    JSG_REQUIRE(iv.size() != 0, DOMOperationError, "AES-GCM IV must not be empty.");

    int tagLength = algorithm.tagLength.orDefault(128);
    validateAesGcmTagLength(tagLength);

    JSG_REQUIRE(cipherText.size() >= tagLength / 8, DOMOperationError,
        "Ciphertext length of ", cipherText.size() * 8, " bits must be greater than or equal to "
        "the size of the AES-GCM tag length of ", tagLength, " bits.");

    auto additionalData = algorithm.additionalData.orDefault(kj::Array<kj::byte>()).asPtr();

    auto cipherCtx = makeCipherContext();
    KJ_ASSERT(cipherCtx.get() != nullptr);

    auto type = lookupAesGcmType(keyData.size() * 8);

    OSSLCALL(EVP_DecryptInit_ex(cipherCtx.get(), type, nullptr, nullptr, nullptr));
    OSSLCALL(EVP_CIPHER_CTX_ctrl(cipherCtx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                 iv.size(), nullptr));
    OSSLCALL(EVP_DecryptInit_ex(cipherCtx.get(), nullptr, nullptr, keyData.begin(),
                                iv.begin()));

    int plainSize = 0;

    if (additionalData.size() > 0) {
      OSSLCALL(EVP_DecryptUpdate(cipherCtx.get(), nullptr, &plainSize,
                                 additionalData.begin(), additionalData.size()));
      plainSize = 0;
    }

    auto actualCipherText = cipherText.slice(0, cipherText.size() - tagLength / 8);
    auto tagText = cipherText.slice(actualCipherText.size(), cipherText.size());

    auto plainText = kj::heapArray<kj::byte>(actualCipherText.size());

    // Perform the actual decryption.
    OSSLCALL(EVP_DecryptUpdate(cipherCtx.get(), plainText.begin(), &plainSize,
                               actualCipherText.begin(), actualCipherText.size()));
    KJ_ASSERT(plainSize == plainText.size());

    // NOTE: We const_cast tagText here. EVP_CIPHER_CTX_ctrl() is used to set various
    //   cipher-specific parameters, not just the GCM tag. Because of this, it takes its pointer
    //   parameter as a void*, thus the const_cast. This is safe because tagText points to a
    //   BufferSource allocated on V8's heap, and we know that OpenSSL does not modify the tag. (If
    //   it did, the W3C crypto tests would fail.)
    //
    //   This little hack seems like a lesser evil than accepting the plaintext as mutable in every
    //   decrypt implementation function interface.
    OSSLCALL(EVP_CIPHER_CTX_ctrl(cipherCtx.get(), EVP_CTRL_GCM_SET_TAG, tagLength / 8,
                                 const_cast<kj::byte*>(tagText.begin())));

    plainSize += decryptFinalHelper(getAlgorithmName(), actualCipherText.size(), plainSize,
        cipherCtx.get(), plainText.begin() + plainSize);
    KJ_ASSERT(plainSize == plainText.size());

    return plainText;
  }
};

class AesCbcKey final: public AesKeyBase {
public:
  explicit AesCbcKey(kj::Array<kj::byte> keyData, CryptoKey::AesKeyAlgorithm keyAlgorithm,
                     bool extractable, CryptoKeyUsageSet usages)
      : AesKeyBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable, usages) {}

private:
  kj::Array<kj::byte> encrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    kj::ArrayPtr<kj::byte> iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError,
        "Missing field \"iv\" in \"algorithm\".");

    JSG_REQUIRE(iv.size() == 16, DOMOperationError, "AES-CBC IV must be 16 bytes long (provided ",
        iv.size(), " bytes).");

    auto cipherCtx = makeCipherContext();
    auto type = lookupAesCbcType(keyData.size() * 8);

    // Set up the cipher context with the initialization vector.
    OSSLCALL(EVP_EncryptInit_ex(cipherCtx.get(), type, nullptr, keyData.begin(),
                                iv.begin()));

    auto blockSize = EVP_CIPHER_CTX_block_size(cipherCtx.get());
    size_t paddingSize = blockSize - (plainText.size() % blockSize);
    auto cipherText = kj::heapArray<kj::byte>(plainText.size() + paddingSize);

    // Perform the actual encryption.
    //
    // Note: We don't worry about PKCS padding (see RFC2315 section 10.3 step 2) because BoringSSL
    //   takes care of it for us by default in EVP_EncryptFinal_ex().

    int cipherSize = 0;
    OSSLCALL(EVP_EncryptUpdate(cipherCtx.get(), cipherText.begin(), &cipherSize,
                               plainText.begin(), plainText.size()));
    KJ_ASSERT(cipherSize <= cipherText.size(), "buffer overrun");

    KJ_ASSERT(cipherSize + blockSize <= cipherText.size(), "imminent buffer overrun");
    int finalCipherSize = 0;
    OSSLCALL(EVP_EncryptFinal_ex(cipherCtx.get(), cipherText.begin() + cipherSize,
                                 &finalCipherSize));
    cipherSize += finalCipherSize;
    KJ_ASSERT(cipherSize == cipherText.size(), "buffer overrun");

    return cipherText;
  }

  kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    kj::ArrayPtr<kj::byte> iv = JSG_REQUIRE_NONNULL(algorithm.iv, TypeError,
        "Missing field \"iv\" in \"algorithm\".");

    JSG_REQUIRE(iv.size() == 16, DOMOperationError,
        "AES-CBC IV must be 16 bytes long (provided ", iv.size(), ").");

    auto cipherCtx = makeCipherContext();
    KJ_ASSERT(cipherCtx.get() != nullptr);

    auto type = lookupAesCbcType(keyData.size() * 8);

    // Set up the cipher context with the initialization vector.
    OSSLCALL(EVP_DecryptInit_ex(cipherCtx.get(), type, nullptr, keyData.begin(),
                                iv.begin()));

    int plainSize = 0;
    auto blockSize = EVP_CIPHER_CTX_block_size(cipherCtx.get());

    auto plainText = kj::heapArray<kj::byte>(cipherText.size() + ((blockSize > 1) ? blockSize : 0));

    // Perform the actual decryption.
    OSSLCALL(EVP_DecryptUpdate(cipherCtx.get(), plainText.begin(), &plainSize,
                               cipherText.begin(), cipherText.size()));
    KJ_ASSERT(plainSize + ((blockSize > 1) ? blockSize : 0) <= plainText.size());

    plainSize += decryptFinalHelper(getAlgorithmName(), cipherText.size(), plainSize,
        cipherCtx.get(), plainText.begin() + plainSize);
    KJ_ASSERT(plainSize <= plainText.size());

    // TODO(perf): Avoid this copy, see comment in the encrypt implementation functions.
    return kj::heapArray(plainText.begin(), plainSize);
  }
};

class AesCtrKey final: public AesKeyBase {
  static constexpr size_t expectedCounterByteSize = 16;

public:
  explicit AesCtrKey(kj::Array<kj::byte> keyData, CryptoKey::AesKeyAlgorithm keyAlgorithm,
                     bool extractable, CryptoKeyUsageSet usages)
      : AesKeyBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable, usages) {}

  kj::Array<kj::byte> encrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> plainText) const override {
    return encryptOrDecrypt(kj::mv(algorithm), plainText);
  }

  kj::Array<kj::byte> decrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> cipherText) const override {
    return encryptOrDecrypt(kj::mv(algorithm), cipherText);
  }

protected:
  static const EVP_CIPHER& lookupAesType(size_t keyLengthBytes) {
    switch (keyLengthBytes) {
      case 16: return *EVP_aes_128_ctr();
      // NOTE: FWIW Chrome intentionally doesn't support 192 (http://crbug.com/533699) & at one
      //   point in removal of the 192 variant was scheduled for removal from BoringSSL. However, we
      //   do support it for completeness (as does Firefox).
      case 24: return *EVP_aes_192_ctr();
      case 32: return *EVP_aes_256_ctr();
    }
    KJ_FAIL_ASSERT("CryptoKey has invalid data length");
  }

  kj::Array<kj::byte> encryptOrDecrypt(
      SubtleCrypto::EncryptAlgorithm&& algorithm, kj::ArrayPtr<const kj::byte> data) const {
    auto& counter = JSG_REQUIRE_NONNULL(algorithm.counter, TypeError,
        "Missing \"counter\" member in \"algorithm\".");
    JSG_REQUIRE(counter.size() == expectedCounterByteSize, DOMOperationError,
        "Counter must have length of 16 bytes (provided ", counter.size(), ").");

    auto& counterBitLength = JSG_REQUIRE_NONNULL(algorithm.length, TypeError,
        "Missing \"length\" member in \"algorithm\".");

    // Web IDL defines an octet as [0, 255] which explains why the spec here only calls out != 0 and
    // <= 128, which implies the intended range must be [1, 128] which is what we enforce here.
    // However, we check > 0 instead of != 0 because we don't enforce the bounds of an octet when
    // converting from JS. If we were to ever add support for annotations into JSG (specifically
    // EnforceRange), then we'd have enforcement way before this that counterBitLength is in the
    // [0, 255] range:
    //   * https://heycam.github.io/webidl/#EnforceRange,
    //   * https://heycam.github.io/webidl/#es-octet
    //   * https://heycam.github.io/webidl/#abstract-opdef-converttoint
    JSG_REQUIRE(counterBitLength > 0 && counterBitLength <= 128,
        DOMOperationError, "Invalid counter of ", counterBitLength, " bits length provided.");

    const auto& cipher = lookupAesType(keyData.size());

    kj::Vector<kj::byte> result;
    // The output of AES-CTR is the same size as the input.
    result.resize(data.size());

    auto numCounterValues = newBignum();
    JSG_REQUIRE(BN_lshift(numCounterValues.get(), BN_value_one(), counterBitLength),
        InternalDOMOperationError, "Error doing ", getAlgorithmName(), " encrypt/decrypt",
        internalDescribeOpensslErrors());

    auto currentCounter = getCounter(counter.asPtr(), counterBitLength);

    // Now figure out how many AES blocks we'll process/how many times to increment the counter.
    auto numOutputBlocks = newBignum();
    JSG_REQUIRE(BN_set_word(numOutputBlocks.get(), integerCeilDivision(result.size(),
        static_cast<size_t>(AES_BLOCK_SIZE))), InternalDOMOperationError, "Error doing ",
        getAlgorithmName(), " encrypt/decrypt", internalDescribeOpensslErrors());

    JSG_REQUIRE(BN_cmp(numOutputBlocks.get(), numCounterValues.get()) <= 0,
        DOMOperationError, "Counter block values will repeat", tryDescribeOpensslErrors());

    auto numBlocksUntilReset = newBignum();
    // The number of blocks that can be encrypted without overflowing the counter. Subsequent
    // blocks will need to reset the counter back to 0. BN_sub's signature is (result, a, b) and
    // evaluates result = a - b. BN_sub documentation says an error happens on allocation failure
    // but I can't find any evidence there's any such allocation & the errors seem to be a result
    // of internal errors.
    JSG_REQUIRE(BN_sub(numBlocksUntilReset.get(), numCounterValues.get(), currentCounter.get()),
        InternalDOMOperationError, "Error doing ", getAlgorithmName(), " encrypt/decrypt",
        internalDescribeOpensslErrors());

    if (BN_cmp(numBlocksUntilReset.get(), numOutputBlocks.get()) >= 0) {
      // If the counter doesn't need any wrapping, can evaluate this as a single call.
      process(&cipher, data, counter, result.asPtr());
      return result.releaseAsArray();
    }

    // Need this to be done in 2 parts using the current counter block and then resetting the
    // counter portion of the block back to zero.
    auto inputSizePart1 = BN_get_word(numBlocksUntilReset.get()) * AES_BLOCK_SIZE;

    process(&cipher, data.slice(0, inputSizePart1), counter, result.asPtr());

    // Zero the counter bits of the block. Chromium creates a copy but we own our buffer.
    {
      KJ_DASSERT(counterBitLength / 8 <= expectedCounterByteSize);

      auto remainder = counterBitLength % 8;
      auto idx = expectedCounterByteSize - counterBitLength / 8;
      memset(counter.begin() + idx, 0, counterBitLength / 8);
      if (remainder) {
        counter[idx - 1] &= 0xFF << remainder;
      }
    }

    process(&cipher, data.slice(inputSizePart1, data.size()), counter, result.slice(
        inputSizePart1, result.size()));

    return result.releaseAsArray();
  }

private:
  UniqueBignum getCounter(kj::ArrayPtr<kj::byte> counterBlock,
      const unsigned counterBitLength) const {
    // See GetCounter from https://chromium.googlesource.com/chromium/src/+/refs/tags/91.0.4458.2/components/webcrypto/algorithms/aes_ctr.cc#86
    // The counter is the rightmost "counterBitLength" of the block as a big-endian number.
    KJ_DASSERT(counterBlock.size() == expectedCounterByteSize);

    auto result = newBignum();

    auto remainderBits = counterBitLength % 8;
    if (remainderBits == 0) {
      // Multiple of 8 bits, then can pass the remainder to BN_bin2bn (binary to bignum).
      auto byteLength = counterBitLength / 8;
      auto remainingCounter = counterBlock.slice(expectedCounterByteSize - byteLength,
          counterBlock.size());
      JSG_REQUIRE(result.get() == BN_bin2bn(remainingCounter.asBytes().begin(),
          byteLength, result.get()), InternalDOMOperationError, "Error doing ", getAlgorithmName(),
          " encrypt/decrypt", internalDescribeOpensslErrors());
      return result;
    }

    // Convert the counter but zero out the topmost bits so that we can convert to bignum from a
    // byte stream. Chromium creates a copy here but that's because they only have a const only view
    // of the data but in our WebCrypto implementation we have a non-const view of the underlying
    // counter buffer (in fact encrypt/decrypt explicitly gives us ownership of that buffer).
    auto byteLength = integerCeilDivision(counterBitLength, 8u);
    KJ_DASSERT(byteLength > 0, counterBitLength, remainderBits);
    KJ_DASSERT(byteLength <= expectedCounterByteSize, counterBitLength, counterBlock.size());

    auto counterToProcess = counterBlock.slice(expectedCounterByteSize - byteLength,
        counterBlock.size());
    auto previous = counterToProcess[0];
    counterToProcess[0] &= ~(0xFF << remainderBits);
    KJ_DEFER(counterToProcess[0] = previous);
    // We temporarily modify the counter to construct the BIGNUM & this undoes it. It's a safe
    // operation because we own the buffer. Technically the restoration isn't even strictly
    // necessary because this buffer isn't used any more after this.

    JSG_REQUIRE(result.get() == BN_bin2bn(counterToProcess.begin(), counterToProcess.size(),
        result.get()), InternalDOMOperationError, "Error doing ", getAlgorithmName(),
        " encrypt/decrypt", internalDescribeOpensslErrors());

    return result;
  }

  void process(const EVP_CIPHER* cipher, kj::ArrayPtr<const kj::byte> input,
      kj::ArrayPtr<kj::byte> counter, kj::ArrayPtr<kj::byte> output) const {
    // Workers are limited to 128MB so this isn't actually a realistic concern, but sanity check.
    JSG_REQUIRE(input.size() < INT_MAX, DOMOperationError, "Input is too large to encrypt.");

    auto cipherContext = makeCipherContext();
    // For CTR, it really does not matter whether we are encrypting or decrypting, so set enc to 0.
    JSG_REQUIRE(EVP_CipherInit_ex(cipherContext.get(), cipher, nullptr,
        keyData.asBytes().begin(), counter.asBytes().begin(), 0),
        InternalDOMOperationError, "Error doing ", getAlgorithmName(), " encrypt/decrypt",
        internalDescribeOpensslErrors());

    int outputLength = 0;
    JSG_REQUIRE(EVP_CipherUpdate(cipherContext.get(), output.begin(), &outputLength,
        input.asBytes().begin(), input.size()), InternalDOMOperationError, "Error doing ",
        getAlgorithmName(), " encrypt/decrypt", internalDescribeOpensslErrors());

    KJ_DASSERT(outputLength >= 0 && outputLength <= output.size(), outputLength, output.size());

    int finalOutputChunkLength = 0;
    auto finalizationBuffer = output.slice(outputLength, output.size()).asBytes().begin();
    JSG_REQUIRE(EVP_CipherFinal_ex(cipherContext.get(), finalizationBuffer,
        &finalOutputChunkLength), InternalDOMOperationError, "Error doing ", getAlgorithmName(),
        " encrypt/decrypt", internalDescribeOpensslErrors());

    KJ_DASSERT(finalOutputChunkLength >= 0 && finalOutputChunkLength <= output.size(),
        finalOutputChunkLength, output.size());

    JSG_REQUIRE(static_cast<size_t>(outputLength) + static_cast<size_t>(finalOutputChunkLength) ==
        input.size(), InternalDOMOperationError, "Error doing ", getAlgorithmName(),
        " encrypt/decrypt.");
  }
};

class AesKwKey final: public AesKeyBase {
public:
  explicit AesKwKey(kj::Array<kj::byte> keyData, CryptoKey::AesKeyAlgorithm keyAlgorithm,
                    bool extractable, CryptoKeyUsageSet usages)
      : AesKeyBase(kj::mv(keyData), kj::mv(keyAlgorithm), extractable, usages) {}

  kj::Array<kj::byte> wrapKey(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> unwrappedKey) const override {
    // Resources used to implement this:
    // https://www.ietf.org/rfc/rfc3394.txt
    // https://chromium.googlesource.com/chromium/src/+/refs/tags/91.0.4458.2/components/webcrypto/algorithms/aes_kw.cc

    JSG_REQUIRE((unwrappedKey.size() % 8) == 0, DOMOperationError,
        "Unwrapped key bit length must be a multiple of 64 bits but unwrapped key has a length of ",
        unwrappedKey.size() * 8, " bits.");

    JSG_REQUIRE(unwrappedKey.size() >= 16 && unwrappedKey.size() <= SIZE_MAX - 8, DOMOperationError,
        "Unwrapped key has length ", unwrappedKey.size(), " bytes but it should be greater than or "
        "equal to 16 and less than or equal to ", SIZE_MAX - 8);

    kj::Vector<kj::byte> wrapped(unwrappedKey.size() + 8);
    wrapped.resize(unwrappedKey.size() + 8);
    // Wrapping adds 8 bytes of overhead for storing the IV which we check on decryption.

    AES_KEY aesKey;
    JSG_REQUIRE(0 == AES_set_encrypt_key(keyData.begin(), keyData.size() * 8, &aesKey),
        InternalDOMOperationError, "Error doing ", getAlgorithmName(), " key wrapping",
        internalDescribeOpensslErrors());

    JSG_REQUIRE(wrapped.size() == AES_wrap_key(&aesKey, nullptr, wrapped.begin(),
        unwrappedKey.begin(), unwrappedKey.size()), DOMOperationError, getAlgorithmName(),
        " key wrapping failed", tryDescribeOpensslErrors());

    return wrapped.releaseAsArray();
  }

  kj::Array<kj::byte> unwrapKey(SubtleCrypto::EncryptAlgorithm&& algorithm,
      kj::ArrayPtr<const kj::byte> wrappedKey) const override {
    // Resources used to implement this:
    // https://www.ietf.org/rfc/rfc3394.txt
    // https://chromium.googlesource.com/chromium/src/+/refs/tags/91.0.4458.2/components/webcrypto/algorithms/aes_kw.cc

    JSG_REQUIRE((wrappedKey.size() % 8) == 0, DOMOperationError,
        "Provided a wrapped key to unwrap that is ", wrappedKey.size() * 8,
        " bits which isn't a multiple of 64 bits.");

    JSG_REQUIRE(wrappedKey.size() >= 24, DOMOperationError,
        "Provided a wrapped key to unwrap this is ", wrappedKey.size() * 8,
        " bits that is less than the minimal length of 192 bits.");

    kj::Vector<kj::byte> unwrapped(wrappedKey.size() - 8);
    // Key wrap adds 8 bytes of overhead because it mixes in the IV.
    unwrapped.resize(wrappedKey.size() - 8);

    AES_KEY aesKey;
    JSG_REQUIRE(0 == AES_set_decrypt_key(keyData.begin(), keyData.size() * 8, &aesKey),
        InternalDOMOperationError, "Error doing ", getAlgorithmName(), " key unwrapping",
        internalDescribeOpensslErrors());

    // null for the IV value here will tell OpenSSL to validate using the default IV from RFC3394.
    // https://github.com/openssl/openssl/blob/13a574d8bb2523181f8150de49bc041c9841f59d/crypto/modes/wrap128.c
    JSG_REQUIRE(unwrapped.size() == AES_unwrap_key(&aesKey, nullptr, unwrapped.begin(),
        wrappedKey.begin(), wrappedKey.size()), DOMOperationError, getAlgorithmName(),
        " key unwrapping failed", tryDescribeOpensslErrors());

    return unwrapped.releaseAsArray();
  }
};

CryptoKeyUsageSet validateAesUsages(CryptoKeyUsageSet::Context ctx, kj::StringPtr normalizedName,
                                    kj::ArrayPtr<const kj::String> keyUsages) {
  // AES-CTR, AES-CBC, AES-GCM, and AES-KW all share the same logic for operations, with the only
  // difference being the valid usages.
  CryptoKeyUsageSet validUsages = CryptoKeyUsageSet::wrapKey() | CryptoKeyUsageSet::unwrapKey();
  if (normalizedName != "AES-KW") {
    validUsages |= CryptoKeyUsageSet::encrypt() | CryptoKeyUsageSet::decrypt();
  }
  return CryptoKeyUsageSet::validate(normalizedName, ctx, keyUsages, validUsages);
}

}  // namespace

kj::OneOf<jsg::Ref<CryptoKey>, CryptoKeyPair> CryptoKey::Impl::generateAes(
      kj::StringPtr normalizedName,
      SubtleCrypto::GenerateKeyAlgorithm&& algorithm, bool extractable,
      kj::ArrayPtr<const kj::String> keyUsages) {
  CryptoKeyUsageSet usages =
      validateAesUsages(CryptoKeyUsageSet::Context::generate, normalizedName, keyUsages);

  auto length = JSG_REQUIRE_NONNULL(algorithm.length, TypeError,
      "Missing field \"length\" in \"algorithm\".");

  switch (length) {
    case 128:
    case 192:
    case 256: break;
    default:
      JSG_FAIL_REQUIRE(DOMOperationError,
          "Generated AES key length must be 128, 192, or 256 bits but requested ", length, ".");
  }

  auto keyDataArray = kj::heapArray<kj::byte>(length / 8);
  IoContext::current().getEntropySource().generate(keyDataArray);

  auto keyAlgorithm = CryptoKey::AesKeyAlgorithm{normalizedName, static_cast<uint16_t>(length)};

  kj::Own<CryptoKey::Impl> keyImpl;

  if (normalizedName == "AES-GCM") {
    keyImpl = kj::heap<AesGcmKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-CBC") {
    keyImpl = kj::heap<AesCbcKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-CTR") {
    keyImpl = kj::heap<AesCtrKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-KW") {
    keyImpl = kj::heap<AesKwKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, normalizedName, " key generation not supported.");
  }

  return jsg::alloc<CryptoKey>(kj::mv(keyImpl));
}

kj::Own<CryptoKey::Impl> CryptoKey::Impl::importAes(
    kj::StringPtr normalizedName, kj::StringPtr format,
    SubtleCrypto::ImportKeyData keyData,
    SubtleCrypto::ImportKeyAlgorithm&& algorithm, bool extractable,
    kj::ArrayPtr<const kj::String> keyUsages) {
  CryptoKeyUsageSet usages =
      validateAesUsages(CryptoKeyUsageSet::Context::importSecret, normalizedName, keyUsages);

  kj::Array<kj::byte> keyDataArray;

  if (format == "raw") {
    // NOTE: Checked in SubtleCrypto::importKey().
    keyDataArray = kj::mv(keyData.get<kj::Array<kj::byte>>());
    switch (keyDataArray.size() * 8) {
      case 128:
      case 192:
      case 256: break;
      default:
        JSG_FAIL_REQUIRE(DOMDataError,
            "Imported AES key length must be 128, 192, or 256 bits but provided ",
            keyDataArray.size() * 8, ".");
    }
  } else if (format == "jwk") {
    auto aesMode = normalizedName.slice(4);

    auto& keyDataJwk = keyData.get<SubtleCrypto::JsonWebKey>();
    JSG_REQUIRE(keyDataJwk.kty == "oct", DOMDataError,
        "Symmetric \"jwk\" key import requires a JSON Web Key with Key Type parameter "
        "\"kty\" equal to \"oct\" (encountered \"", keyDataJwk.kty, "\").");
    // https://www.rfc-editor.org/rfc/rfc7518.txt Section 6.1
    keyDataArray = UNWRAP_JWK_BIGNUM(kj::mv(keyDataJwk.k), DOMDataError,
        "Symmetric \"jwk\" key import requires a base64Url encoding of the key.");

    switch (keyDataArray.size() * 8) {
      case 128:
      case 192:
      case 256:
        KJ_IF_MAYBE(alg, keyDataJwk.alg) {
          auto expectedAlg = kj::str("A", keyDataArray.size() * 8, aesMode);
          JSG_REQUIRE(*alg == expectedAlg, DOMDataError,
              "Symmetric \"jwk\" key contains invalid \"alg\" value \"", *alg, "\", expected \"",
              expectedAlg, "\".");
        }
        break;
      default:
        JSG_FAIL_REQUIRE(DOMDataError,
            "Imported AES key length must be 128, 192, or 256 bits but provided ",
            keyDataArray.size() * 8, ".");
    }

    if (keyUsages.size() != 0) {
      KJ_IF_MAYBE(u, keyDataJwk.use) {
        JSG_REQUIRE(*u == "enc", DOMDataError,
            "Symmetric \"jwk\" key must have a \"use\" of \"enc\", not \"", *u, "\".");
      }
    }

    KJ_IF_MAYBE(ops, keyDataJwk.key_ops) {
      std::sort(ops->begin(), ops->end());
      // Don't want to use the trick above to use a red-black tree because that constructs the set
      // once ever for the process, but this path is dependent on user input. Could write things
      // without the sort but it makes the enforcement from Section 4.2 below a 1-liner.

      auto duplicate = std::adjacent_find(ops->begin(), ops->end());
      JSG_REQUIRE(duplicate == ops->end(), DOMDataError,
          "Symmetric \"jwk\" key contains duplicate value \"", *duplicate, "\", in \"key_op\".");
      // https://tools.ietf.org/html/rfc7517#section-4.2 - no duplicate values in key_ops.

      for (const auto& usage: keyUsages) {
        JSG_REQUIRE(std::binary_search(ops->begin(), ops->end(), usage), DOMDataError,
            "\"jwk\" key missing usage \"", usage, "\", in \"key_ops\".");
      }
    }

    // TODO(conform/review): How should this from the standard:
    //     > The "use" and "key_ops" JWK members SHOULD NOT be used together;
    //     > however, if both are used, the information they convey MUST be
    //     > consistent
    //   be interpreted? What constitutes "inconsistentcy"? Is that implicit in enforcing that "enc"
    //   must be the value for `use'? Or is there something else?

    KJ_IF_MAYBE(e, keyDataJwk.ext) {
      JSG_REQUIRE(*e || !extractable, DOMDataError,
          "\"jwk\" key has value \"", *e ? "true" : "false", "\", for \"ext\" that is incompatible "
          "with import extractability value \"", extractable ? "true" : "false", "\".");
    }
  } else {
    JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unrecognized key import format \"", format, "\".");
  }

  auto keySize = keyDataArray.size() * 8;
  KJ_ASSERT(keySize == 128 || keySize == 192 || keySize == 256);

  auto keyAlgorithm = CryptoKey::AesKeyAlgorithm{normalizedName, static_cast<uint16_t>(keySize)};

  if (normalizedName == "AES-GCM") {
    return kj::heap<AesGcmKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-CBC") {
    return kj::heap<AesCbcKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-CTR") {
    return kj::heap<AesCtrKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  } else if (normalizedName == "AES-KW") {
    return kj::heap<AesKwKey>(kj::mv(keyDataArray), kj::mv(keyAlgorithm), extractable, usages);
  }

  JSG_FAIL_REQUIRE(DOMNotSupportedError, "Unsupported algorithm \"", normalizedName,
      "\" to import.");
}

}  // namespace workerd::api
