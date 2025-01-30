// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>

namespace workerd {

// If an exception is thrown for exceeding memory limits, it will contain this detail.
constexpr kj::Exception::DetailTypeId MEMORY_LIMIT_DETAIL_ID = 0xbaf76dd7ce5bd8cfull;

}  // namespace workerd
