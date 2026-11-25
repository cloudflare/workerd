// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/util/autogate.h>

namespace workerd::autogate_bridge {

size_t autogate_count() {
  return util::getAutogateKeys().size();
}

bool autogate_is_enabled(size_t index) {
  return util::Autogate::isEnabled(util::getAutogateKeys()[index]);
}

::rust::Str autogate_name(size_t index) {
  // KJ_STRINGIFY(AutogateKey) gives a static string.
  kj::StringPtr name = kj::toCharSequence(util::getAutogateKeys()[index]);
  return ::rust::Str(name.begin(), name.size());
}

}  // namespace workerd::autogate_bridge
