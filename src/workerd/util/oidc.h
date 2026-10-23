// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/string.h>

namespace workerd {

// JWT StringOrURI, bounded to 2048 UTF-8 bytes and excluding whitespace-only audiences.
// Values containing ':' must follow RFC 3986 URI syntax. Validation never normalizes or fetches.
bool isValidOidcAudience(kj::StringPtr value);

}  // namespace workerd
