#pragma once

#include "impl.h"
#include "keys.h"

namespace workerd::api {

// TODO(soon): All of the JWK conversion logic will be moved in here
// soon. Currently this only covers conversion with ncrypto keys not
// kj::Own held keys.

SubtleCrypto::JsonWebKey toJwk(
    const ncrypto::EVPKeyPointer& key, KeyType keyType = KeyType::PUBLIC);

ncrypto::EVPKeyPointer fromJwk(
    const SubtleCrypto::JsonWebKey& jwk, KeyType keyType = KeyType::PUBLIC);

}  // namespace workerd::api
