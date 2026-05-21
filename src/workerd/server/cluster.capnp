# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xbd0c09739255a698;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::cluster");
$Cxx.allowCancellation;

struct VatId {
  publicKey @0 :Data;
  # 32-byte raw X25519 public key.
}

# The following are stubs for v1. They are not used because canIntroduceTo() returns
# false (the default), so the RPC system falls back to proxying for any cross-peer
# capability passing. Cap'n Proto itself has not yet fully implemented Level 3, so
# leaving these empty is fine.
struct ThirdPartyToContact {}
struct ThirdPartyToAwait {}
struct ThirdPartyCompletion {}

# Level 4 (Join) is not supported.
struct JoinResult {}
