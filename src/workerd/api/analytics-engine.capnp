# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x8fe697b0d6269a23;

$import "/capnp/c++.capnp".namespace("workerd::api");

# ========================================================================================
# DO NOT MODIFY BELOW THIS COMMENT -- except if copying from the authoritative version.
#
# This protocol belongs to the Data team. Any modifications must be made in the data/schemas
# repo, and then copied here *after* it has been merged there. Any annotations relating to
# languages other than C++ should be removed after copying.
# ========================================================================================

struct AnalyticsEngineEvent {
  # Canonical struct for sending events to Analytics Engine

  # Cloudflare numeric account ID.
  accountId @0 :Int64;

  # Name of the dataset this belongs to.
  # For instance, weather measurements or website visits.
  dataset @1 :Data;

  # For tracking breaking changes to the schema
  schemaVersion @2 :Int64;

  # Secondary piece of data by which to sample and index events.
  # For example, Cloudflare zone ID or Waiting Room ID.
  # Cannot be longer than 64 bytes.
  # We anticipate allowing users to configure multiple data dimensions
  # in future, requiring an `index2` and maybe `index3`, hence the name
  # ending with `1`.
  index1 @3 :Data;

  # Timestamp in nanoseconds
  timestamp @4 :Int64;

  double1 @5 :Float64;
  double2 @6 :Float64;
  double3 @7 :Float64;
  double4 @8 :Float64;
  double5 @9 :Float64;
  double6 @10 :Float64;
  double7 @11 :Float64;
  double8 @12 :Float64;
  double9 @13 :Float64;
  double10 @14 :Float64;
  double11 @15 :Float64;
  double12 @16 :Float64;
  double13 @17 :Float64;
  double14 @18 :Float64;
  double15 @19 :Float64;
  double16 @20 :Float64;
  double17 @21 :Float64;
  double18 @22 :Float64;
  double19 @23 :Float64;
  double20 @24 :Float64;

  # The total length of all blobs cannot exceed 256*20 = 5120 bytes.
  blob1 @25 :Data;
  blob2 @26 :Data;
  blob3 @27 :Data;
  blob4 @28 :Data;
  blob5 @29 :Data;
  blob6 @30 :Data;
  blob7 @31 :Data;
  blob8 @32 :Data;
  blob9 @33 :Data;
  blob10 @34 :Data;
  blob11 @35 :Data;
  blob12 @36 :Data;
  blob13 @37 :Data;
  blob14 @38 :Data;
  blob15 @39 :Data;
  blob16 @40 :Data;
  blob17 @41 :Data;
  blob18 @42 :Data;
  blob19 @43 :Data;
  blob20 @44 :Data;
}
