#pragma once

#include <kj/common.h>

namespace workerd::api {

bool verifySpkac(kj::ArrayPtr<const kj::byte> input);

kj::Maybe<kj::Array<kj::byte>> exportPublicKey(kj::ArrayPtr<const kj::byte> input);

kj::Maybe<kj::Array<kj::byte>> exportChallenge(kj::ArrayPtr<const kj::byte> input);

}  // namespace workerd::api
