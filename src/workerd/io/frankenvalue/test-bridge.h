// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>

// What test_bridge.rs calls back in C++, defined in test-bridge.c++.
namespace workerd::frankenvalue::test {

// Moves `value` into consumeByValue() (test-bridge-state.h).
void consume_frankenvalue(kj::Own<Frankenvalue> value);

}  // namespace workerd::frankenvalue::test
