// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/string.h>

namespace workerd::api::htmlrewriter {

constexpr size_t MAX_SELECTOR_COMPLEXITY = 256;

// Returns true when parsing or compiling the selector would require recursion beyond the limit.
// This only scans CSS lexical structure; lol-html remains responsible for syntax validation.
bool selectorExceedsComplexityLimit(kj::StringPtr selector);

}  // namespace workerd::api::htmlrewriter
