# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xb4c39b5e1a2369f2;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::tools");

struct FlagInfo @0x9c1863b221b3e2aa {
  field @0 :Text;
  enableFlag @1 :Text;
  disableFlag @2 :Text;
  date @3 :Text;
  dateSource @4 :Text;
}

struct FlagInfoList @0xd2088d55b2e71c1f {
  flags @0 :List(FlagInfo);
}
