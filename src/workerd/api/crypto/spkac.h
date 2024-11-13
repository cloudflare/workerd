#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/common.h>

namespace workerd::api {

bool verifySpkac(kj::ArrayPtr<const kj::byte> input);

kj::Maybe<jsg::BufferSource> exportPublicKey(jsg::Lock& js, kj::ArrayPtr<const kj::byte> input);

kj::Maybe<jsg::BufferSource> exportChallenge(jsg::Lock& js, kj::ArrayPtr<const kj::byte> input);

}  // namespace workerd::api
