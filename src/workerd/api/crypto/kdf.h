#pragma once

#include <kj/common.h>

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

}
