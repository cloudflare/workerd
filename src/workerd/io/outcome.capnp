# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xe27bb9203d5e02a8;

$import "/capnp/c++.capnp".namespace("workerd");

# ========================================================================================
# DO NOT MODIFY BELOW THIS COMMENT -- except if copying from the authoritative version.
#
# This enum is defined as part of the Cloudflare Workers log schemas. The upstream version
# of the enum needs to be updated first, and then changes can be copied here.
# ========================================================================================

enum EventOutcome {
  unknown @0;
  ok @1;
  exception @2;
  exceededCpu @3;
  killSwitch @4;
  daemonDown @5;
  scriptNotFound @6;
  canceled @7;
  exceededMemory @8;
}
