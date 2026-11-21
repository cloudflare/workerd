// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ that the unsafe modules in Rust call. These are Rust JSG shims, which use raw V8 and
// no C++ JSG; see src/rust/jsg/README.md §Safety rules.

#include <rust/cxx.h>

#include <cstdint>

namespace v8 {
class Isolate;
}

namespace workerd::api::unsafe_api {

// Whether the TEST_WORKERD autogate is on.
bool is_test_autogate_enabled();

// What reprl_execute() returns for a program that failed to compile.
constexpr int32_t REPRL_COMPILE_FAILED = -1;

// Runs a Fuzzilli program, `{` + `script` + `}` as a classic script named "reprl", in a
// HandleScope and a verbose v8::TryCatch, so that what it throws is reported and then cleared.
// Returns 0 if it ran and its value converts to an int32, 11 if either threw, and
// REPRL_COMPILE_FAILED if it failed to compile. `isolate` must be locked by this thread, with a
// context entered and no exception pending.
int32_t reprl_execute(v8::Isolate* isolate, ::rust::Slice<const uint8_t> script);

// Resets the coverage edge guards of a build for Fuzzilli, and does nothing in other builds.
void reprl_reset_edge_guards();

}  // namespace workerd::api::unsafe_api
