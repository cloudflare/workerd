# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xd25a546ad8e45f46;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
$Cxx.allowCancellation;

struct JaegerSpan @0x946ed67bd99d1210 {
  traceIdHigh @0 :UInt64;
  traceIdLow @1 :UInt64;
  spanId @2 :UInt64;
  parentSpanId @3 :UInt64;
  flags @4 :UInt8;
}
