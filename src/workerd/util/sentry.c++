// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sentry.h"

#include <workerd/util/entropy.h>
#include <workerd/util/thread-scopes.h>

namespace workerd {

InternalErrorId makeInternalErrorId() {
  InternalErrorId id;
  if (isPredictableModeForTest()) {
    // In testing mode, use content that generates a "0123456789abcdefghijklm" ID:
    for (auto i: kj::indices(id)) {
      id[i] = i;
    }
  } else {
    getEntropy(kj::asBytes(id));
  }
  for (auto i: kj::indices(id)) {
    id[i] = "0123456789abcdefghijklmnopqrstuv"[static_cast<unsigned char>(id[i]) % 32];
  }
  return id;
}

}  // namespace workerd
