// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/common.h>

#include <cstdint>

typedef struct env_md_st EVP_MD;

namespace workerd::api {

// Perform HKDF key derivation.
kj::Maybe<kj::Array<kj::byte>> hkdf(size_t length,
    const EVP_MD* digest,
    kj::ArrayPtr<const kj::byte> key,
    kj::ArrayPtr<const kj::byte> salt,
    kj::ArrayPtr<const kj::byte> info);

// Perform PBKDF2 key derivation.
kj::Maybe<kj::Array<kj::byte>> pbkdf2(size_t length,
    size_t iterations,
    const EVP_MD* digest,
    kj::ArrayPtr<const kj::byte> password,
    kj::ArrayPtr<const kj::byte> salt);

// Perform Scrypt key derivation.
kj::Maybe<kj::Array<kj::byte>> scrypt(size_t length,
    uint32_t N,
    uint32_t r,
    uint32_t p,
    uint32_t maxmem,
    kj::ArrayPtr<const kj::byte> pass,
    kj::ArrayPtr<const kj::byte> salt);

}  // namespace workerd::api
