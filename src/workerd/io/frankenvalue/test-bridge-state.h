// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>

// What the functions of test-bridge.h record, for frankenvalue-ffi-test.c++ to check.
namespace workerd::frankenvalue::test {

// A C++ consumer that takes a Frankenvalue by value, as WorkerStubChannel::getEntrypoint() takes
// props. It keeps the value in `consumed`.
void consumeByValue(Frankenvalue value);

// The value consumeByValue() last took.
extern kj::Maybe<Frankenvalue> consumed;

}  // namespace workerd::frankenvalue::test
