// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <rust/cxx.h>

#include <cstdint>

// The C++ side of the `perfetto` crate's bridge (ffi.rs). Events go to the "workerd" category.
//
// The names are NUL-terminated strings with the NUL, which perfetto keeps by address for the rest
// of the process, as it does C++ string literals.
namespace workerd::perfetto_bridge {

// The address of the category's enabled state: a `std::atomic<uint8_t>` that is nonzero while a
// tracing session records the category. The address is fixed for the life of the process.
uint8_t* category_state();

// A slice on the current thread's track.
void trace_event_begin(::rust::Slice<const uint8_t> name);
void trace_event_end();

// A slice on the track with the given id, a child of the process track.
void trace_event_begin_on_track(::rust::Slice<const uint8_t> name, uint64_t track);
void trace_event_end_on_track(uint64_t track);

}  // namespace workerd::perfetto_bridge
