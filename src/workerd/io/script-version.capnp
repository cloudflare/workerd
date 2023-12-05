# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xd3b6bb739ff1b77a;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");

struct ScriptVersion {
  id @0 :Text;
  # An ID that should be used to uniquely identify this version.
  tag @1 :Text;
  # An optional tag to associate with this version.
  message @2 :Text;
  # An optional message that can be used to describe this version.
}
