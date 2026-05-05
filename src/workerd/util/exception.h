// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>

namespace workerd {

// NOTE: Constants in this file must match `src/rust/worker/exception.rs`

// If an exception is thrown for exceeding CPU time limits, it will contain this detail.
constexpr kj::Exception::DetailTypeId CPU_LIMIT_DETAIL_ID = 0xfdcb787ba4240576ull;
// If an exception is thrown for exceeding memory limits, it will contain this detail.
constexpr kj::Exception::DetailTypeId MEMORY_LIMIT_DETAIL_ID = 0xbaf76dd7ce5bd8cfull;
// If an exception is thrown for worker killed before start, it will contain this detail.
const kj::Exception::DetailTypeId SCRIPT_KILLED_DETAIL_ID = 0xf8935d579c20da70;

}  // namespace workerd
