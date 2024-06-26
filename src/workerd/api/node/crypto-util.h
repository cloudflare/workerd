// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <cstdio>

// TODO: Move to general node/util.h
#ifdef __GNUC__
#define MUST_USE_RESULT __attribute__((warn_unused_result))
#else
#define MUST_USE_RESULT
#endif

// TODO:
namespace workerd::api::node::CryptoUtil {

struct CSPRNGResult {
  const bool ok;
  MUST_USE_RESULT bool is_ok() const { return ok; }
  MUST_USE_RESULT bool is_err() const { return !ok; }
};

// Either succeeds with exactly |length| bytes of cryptographically
// strong pseudo-random data, or fails. This function may block.
// Don't assume anything about the contents of |buffer| on error.
// As a special case, |length == 0| can be used to check if the CSPRNG
// is properly seeded without consuming entropy.
MUST_USE_RESULT CSPRNGResult CSPRNG(void* buffer, size_t length);

}  // namespace workerd::api::node
