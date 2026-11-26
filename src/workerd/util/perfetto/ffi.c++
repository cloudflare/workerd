// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/util/use-perfetto-categories.h>

#include <atomic>

namespace workerd::perfetto_bridge {

#ifdef WORKERD_USE_PERFETTO

namespace {

// Rust reads the state as an `AtomicU8`, which has the same layout.
static_assert(sizeof(std::atomic<uint8_t>) == 1 && std::atomic<uint8_t>::is_always_lock_free);

namespace categories = ::workerd::traces::PERFETTO_TRACK_EVENT_NAMESPACE::internal;

constexpr size_t CATEGORY_INDEX = categories::kConstExprCategoryRegistry.Find("workerd", false);
static_assert(
    CATEGORY_INDEX != ::perfetto::internal::TrackEventCategoryRegistry::kInvalidCategoryIndex);

const char* eventName(::rust::Slice<const uint8_t> name) {
  return reinterpret_cast<const char*>(name.data());
}

}  // namespace

uint8_t* category_state() {
  return reinterpret_cast<uint8_t*>(categories::kCategoryRegistry.GetCategoryState(CATEGORY_INDEX));
}

void trace_event_begin(::rust::Slice<const uint8_t> name) {
  TRACE_EVENT_BEGIN("workerd", ::perfetto::StaticString(eventName(name)));
}

void trace_event_end() {
  TRACE_EVENT_END("workerd");
}

void trace_event_begin_on_track(::rust::Slice<const uint8_t> name, uint64_t track) {
  TRACE_EVENT_BEGIN("workerd", ::perfetto::StaticString(eventName(name)), ::perfetto::Track(track));
}

void trace_event_end_on_track(uint64_t track) {
  TRACE_EVENT_END("workerd", ::perfetto::Track(track));
}

#else  // defined(WORKERD_USE_PERFETTO)

uint8_t* category_state() {
  // Never written, so the category always reads as disabled.
  static std::atomic<uint8_t> disabled{0};
  return reinterpret_cast<uint8_t*>(&disabled);
}

void trace_event_begin(::rust::Slice<const uint8_t> name) {}
void trace_event_end() {}
void trace_event_begin_on_track(::rust::Slice<const uint8_t> name, uint64_t track) {}
void trace_event_end_on_track(uint64_t track) {}

#endif  // defined(WORKERD_USE_PERFETTO)

}  // namespace workerd::perfetto_bridge
