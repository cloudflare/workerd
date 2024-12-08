# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xc40f73be329a38d9;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");

# This file contains trace helper structures. The Trace struct is deliberately not defined here –
# many files just need to have the span/tag definitions available. Since Trace contains interfaces,
# it also causes a large amount of code to be generated in the .capnp.h file, which affects header
# parsing overhead/compile times for every file that depends on the capnp file defining Trace.

struct TagValue {
  union {
    string @0 :Text;
    bool @1 :Bool;
    int64 @2 :Int64;
    float64 @3 :Float64;
  }
}

struct Tag {
  key @0 :Text;
  value @1 :TagValue;
}

