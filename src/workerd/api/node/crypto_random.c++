#include "crypto.h"
#include <v8.h>
#include <openssl/bn.h>
#include <openssl/rand.h>
#include <workerd/jsg/jsg.h>
#include <workerd/api/crypto-impl.h>

namespace workerd::api::node {

bool CryptoImpl::checkPrimeSync(kj::Array<kj::byte> bufferView, uint32_t num_checks) {
  auto candidate = OSSLCALL_OWN(BIGNUM, BN_bin2bn(bufferView.begin(), bufferView.size(), nullptr),
    Error, "Error while checking prime");
  auto ctx = OSSL_NEW(BN_CTX);
  int ret = BN_is_prime_ex(candidate.get(), num_checks, ctx.get(), nullptr);
  JSG_REQUIRE(ret >= 0, Error, "Error while checking prime");
  return ret > 0;
}

}  // namespace workerd::api::node
