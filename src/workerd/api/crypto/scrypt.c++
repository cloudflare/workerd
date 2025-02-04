// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "impl.h"
#include "kdf.h"

#include <ncrypto.h>

namespace workerd::api {

namespace {
template <typename T = const kj::byte>
ncrypto::Buffer<T> ToBuffer(kj::ArrayPtr<T> array) {
  return ncrypto::Buffer<T>(array.begin(), array.size());
}
}  // namespace

kj::Maybe<jsg::BufferSource> scrypt(jsg::Lock& js,
    size_t length,
    uint32_t N,
    uint32_t r,
    uint32_t p,
    uint32_t maxmem,
    kj::ArrayPtr<const kj::byte> pass,
    kj::ArrayPtr<const kj::byte> salt) {
  ncrypto::ClearErrorOnReturn clearErrorOnReturn;
  auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, length);
  ncrypto::Buffer<kj::byte> buf(backing.asArrayPtr().begin(), backing.size());
  ncrypto::Buffer<const char> passbuf{
    .data = reinterpret_cast<const char*>(pass.begin()),
    .len = pass.size(),
  };
  if (ncrypto::scryptInto(passbuf, ToBuffer(salt), N, r, p, maxmem, length, &buf)) {
    return jsg::BufferSource(js, kj::mv(backing));
  }
  return kj::none;
}

}  // namespace workerd::api
