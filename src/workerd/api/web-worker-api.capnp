# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xa8665be3324bbef5;

using Cxx = import "/capnp/c++.capnp";
using Json = import "/capnp/compat/json.capnp";

$Cxx.namespace("workerd::api::experimental");
$Cxx.allowCancellation;

struct CreateWorkerRequest {
  url @0 :Text;
  options @1 :WorkerOptions;
}

struct WorkerOptions {
  type :union $Json.flatten() $Json.discriminator(name="type") {
    classic @0 :Void;
    module @1 :Void;
    wildcard @2 :Void;
  }
  credentials :union $Json.flatten() $Json.discriminator(name="credentials") {
    omit @3 :Void;
    sameOrigin @4 :Void;
    include @5 :Void;
  }
  name @6 :Text;

}

struct CreateWorkerResponse {}
