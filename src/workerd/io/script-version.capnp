# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xd3b6bb739ff1b77a;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");

struct ScriptVersion {
  id :group {
    upper @0 :UInt64;
    # Most significant bits of the UUID.
    lower @1 :UInt64;
    # Least significant bits of the UUID.
  }
  # An optional UUID identifying this version. A null UUID value (where both upper and lower values
  # are 0) can be used to indicate the absence of an ID.
  tag @2 :Text;
  # An optional tag to associate with this version.
  message @3 :Text;
  # An optional message that can be used to describe this version.
}
