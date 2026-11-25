// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of the bridge in glob_filter.rs.

#include <rust/cxx.h>

#include <kj/glob-filter.h>
#include <kj/string.h>

#include <cstdint>

namespace workerd::api {

// Whether `name` matches `pattern`, both bytes, as `kj::GlobFilter` matches them.
inline bool glob_filter_matches(
    ::rust::Slice<const uint8_t> pattern, ::rust::Slice<const uint8_t> name) {
  kj::GlobFilter filter(
      kj::arrayPtr(reinterpret_cast<const char*>(pattern.data()), pattern.size()));
  // matches() takes a NUL-terminated kj::StringPtr, so the name is copied.
  return filter.matches(
      kj::str(kj::arrayPtr(reinterpret_cast<const char*>(name.data()), name.size())));
}

}  // namespace workerd::api
