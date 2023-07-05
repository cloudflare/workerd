// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <kj/string.h>

namespace workerd {

inline kj::String toLowerCopy(kj::StringPtr ptr) {
  auto str = kj::str(ptr);
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

inline kj::String toLowerCopy(kj::ArrayPtr<const char> ptr) {
  auto str = kj::str(ptr);
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

}  // namespace workerd
