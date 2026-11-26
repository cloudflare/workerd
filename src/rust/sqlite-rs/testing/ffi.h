// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <rust/cxx.h>

namespace workerd::rust::sqlite_rs::testing {

// Calls `callback(data)` and returns what KJ logged on this thread meanwhile, each message as
// "<severity>: <text>". The messages are also passed on to be logged as usual.
::rust::Vec<::rust::String> capture_kj_logs(size_t data, ::rust::Fn<void(size_t)> callback);

void install_sqlite_custom_allocator();

}  // namespace workerd::rust::sqlite_rs::testing
