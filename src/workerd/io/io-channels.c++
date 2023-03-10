// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-channels.h"

namespace workerd {

IoChannelFactory::SubrequestMetadata IoChannelFactory::SubrequestMetadata::clone() {
  return SubrequestMetadata {
    .cfBlobJson = cfBlobJson.map([](kj::String& blob) { return kj::str(blob); }),
    .parentSpan = parentSpan.addRef(),
    .featureFlagsForFl = featureFlagsForFl,
  };
}

} // namespace workerd
