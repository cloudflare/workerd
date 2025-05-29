// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

namespace workerd {

// Interface for enforcing various actor related account limits.
// Non-functional in workerd, but used to enforce free tier account limits internally.
class ActorAccountLimits {
 public:
  // Throws if the associated account is no longer allowed to execute sqlite queries
  virtual void requireActorCanExecuteQueries() const {};

  // Allow default equality comparison
  bool operator==(const ActorAccountLimits&) const = default;
};

}  // namespace workerd
