// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "features.h"

#include "worker.h"

namespace workerd {

CompatibilityFlags::Reader FeatureFlags::get(jsg::Lock&) {
  // Note that the jsg::Lock& argument here is not actually used. We require
  // that a jsg::Lock reference is passed in as proof that current() is called
  // from within a valid isolate lock so that the Worker::Api::current()
  // call below will work as expected.
  // TODO(later): Use of Worker::Api::current() here implies that there
  // is only one set of compatibility flags relevant at a time within each thread
  // context. For now that holds true. Later it is possible that may not be the
  // case which will require us to further adapt this model.
  return Worker::Api::current().getFeatureFlags();
}

}  // namespace workerd
