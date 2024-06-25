#include "impl.h"
#include <workerd/api/crypto/kdf.h>
#include <openssl/crypto.h>

namespace workerd::api {

kj::Maybe<kj::Array<kj::byte>> scrypt(size_t length,
                                      uint32_t N,
                                      uint32_t r,
                                      uint32_t p,
                                      uint32_t maxmem,
                                      kj::ArrayPtr<const kj::byte> pass,
                                      kj::ArrayPtr<const kj::byte> salt) {
  auto buf = kj::heapArray<kj::byte>(length);
  if (!EVP_PBE_scrypt(pass.asChars().begin(),
                      pass.size(),
                      salt.begin(),
                      salt.size(),
                      N, r, p, maxmem,
                      buf.begin(),
                      length)) {
    return kj::none;
  }
  return kj::mv(buf);
}

}  // namespace workerd::api
