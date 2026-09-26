#pragma once

#include <rust/cxx.h>

#include <cstddef>
#include <cstdint>

namespace workerd::rust::perfetto_test {

// Whether this build has Perfetto (WORKERD_USE_PERFETTO).
bool perfetto_in_build();

// Starts a PerfettoSession recording `categories` into a temporary file. No-op without Perfetto.
void start_trace(::rust::Str categories);

// Emits C++ trace points that use a flow and a track derived from `address`, and a counter.
void emit_cpp_events(size_t address);

// Stops the session started by start_trace() and returns the serialized trace. Returns an empty
// vector without Perfetto.
::rust::Vec<uint8_t> stop_trace();

}  // namespace workerd::rust::perfetto_test
