#include "crypto-utils.h"

#include <openssl/bio.h>
#include <openssl/pem.h>

namespace workerd {

SslArrayDisposer SslArrayDisposer::INSTANCE;

void SslArrayDisposer::disposeImpl(
    void* firstElement, size_t elementSize, size_t elementCount,
    size_t capacity, void (*destroyElement)(void*)) const {
  OPENSSL_free(firstElement);
}

kj::Maybe<PemData> decodePem(kj::ArrayPtr<const char> text) {
  // TODO(cleanup): Should this be part of the KJ TLS library? We don't technically use it for TLS.
  //   Maybe KJ should have a general crypto library that wraps OpenSSL?

  BIO* bio = BIO_new_mem_buf(const_cast<char*>(text.begin()), text.size());
  KJ_DEFER(BIO_free(bio));

  char* namePtr = nullptr;
  char* headerPtr = nullptr;
  kj::byte* dataPtr = nullptr;
  long dataLen = 0;
  if (!PEM_read_bio(bio, &namePtr, &headerPtr, &dataPtr, &dataLen)) {
    return kj::none;
  }
  kj::Array<char> nameArr(namePtr, strlen(namePtr) + 1, SslArrayDisposer::INSTANCE);
  KJ_DEFER(OPENSSL_free(headerPtr));
  kj::Array<kj::byte> data(dataPtr, dataLen, SslArrayDisposer::INSTANCE);

  return PemData { kj::String(kj::mv(nameArr)), kj::mv(data) };
}

}
