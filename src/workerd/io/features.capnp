# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x8b3d4aaa36221ec9;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");
$Cxx.allowCancellation;

enum Features {
  test @0;
  # A test feature that should never be used in production code.

  # Due to a number of practical limitations on the metrics collection,
  # we do not really want the list of features to grow unbounded over
  # time. At any given point in time we shouldn't be trying to track
  # more than 50 features at a time.
  #
  # Features we are no longer needing to track can and should be removed,
  # just be careful to adjust the index ordinals of the remaining features
  # correctly. In code, be sure to never rely on the ordinal value and
  # instead always use the features enum to ensure that things won't break.

  # We want to determine how users typically read the data from a Blob.
  # The reason is so that we can determine how best to optimize the Blob
  # implementation.
  blobAsArrayBuffer @1;
  blobAsText @2;
  blobAsStream @3;
  blobGetData @4;
}
