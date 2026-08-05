// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd {

// Per-request Cloudflare Access authentication information, surfaced to JS as `ctx.access`.
class AccessInfo: public kj::Refcounted {
 public:
  virtual ~AccessInfo() noexcept(false) = default;

  // The audience claim from the Access JWT. Stable for the lifetime of the request.
  virtual kj::StringPtr getAudience() = 0;

  // Subrequest channel index for the Access identity binding worker. The IoChannelFactory is
  // responsible for injecting per-request props (aud, jwtClaims) when this channel is dispatched.
  // Returns `kj::none` when no identity service is available, causing `ctx.access.getIdentity()`
  // to resolve to `undefined`.
  virtual kj::Maybe<kj::uint> getIdentityServiceChannel() = 0;
};

}  // namespace workerd
