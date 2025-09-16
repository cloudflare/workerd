// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "performance.h"

#include <workerd/io/io-util.h>

#include <kj/encoding.h>

namespace workerd::api {

double Performance::now() {
  // We define performance.now() for compatibility purposes, but due to Spectre concerns it
  // returns exactly what Date.now() returns.
  return dateNow();
}

jsg::JsObject PerformanceEntry::toJSON(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "PerformanceEntry.toJSON is not implemented"_kj);
}

jsg::JsObject PerformanceResourceTiming::toJSON(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "PerformanceResourceTiming.toJSON is not implemented"_kj);
}

void Performance::clearMarks(jsg::Optional<kj::String> name) { /* Intentionally left as no-op */ }
void Performance::clearMeasures(jsg::Optional<kj::String> name) { /* Intentionally left as no-op */
}
void Performance::clearResourceTimings() { /* Intentionally left as no-op */ }

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> Performance::getEntries() {
  return {};
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> Performance::getEntriesByName(
    kj::String name, kj::String type) {
  return {};
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> Performance::getEntriesByType(kj::String type) {
  return {};
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> PerformanceObserverEntryList::getEntries() {
  return {};
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> PerformanceObserverEntryList::getEntriesByType(
    kj::String type) {
  return {};
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> PerformanceObserverEntryList::getEntriesByName(
    kj::String name, jsg::Optional<kj::String> type) {
  return {};
}

jsg::Ref<PerformanceMark> Performance::mark(
    jsg::Lock& js, kj::String name, jsg::Optional<PerformanceMark::Options> options) {
  JSG_FAIL_REQUIRE(Error, "Performance.mark is not implemented");
}

jsg::Ref<PerformanceMeasure> Performance::measure(jsg::Lock& js,
    kj::String measureName,
    kj::OneOf<PerformanceMeasure::Options, kj::String> measureOptionsOrStartMark,
    jsg::Optional<kj::String> maybeEndMark) {
  JSG_FAIL_REQUIRE(Error, "Performance.measure is not implemented");
}

void Performance::setResourceTimingBufferSize(uint32_t size) {
  JSG_FAIL_REQUIRE(Error, "Performance.setResourceTimingBufferSize is not implemented");
}

jsg::Ref<PerformanceObserver> PerformanceObserver::constructor(jsg::Lock& js, Callback callback) {
  return js.alloc<PerformanceObserver>(js, callback);
}

void PerformanceObserver::disconnect() {
  JSG_FAIL_REQUIRE(Error, "PerformanceObserver.disconnect is not implemented");
}

void PerformanceObserver::observe(jsg::Optional<ObserveOptions> options) {
  JSG_FAIL_REQUIRE(Error, "PerformanceObserver.observe is not implemented");
}

kj::Array<jsg::Ref<PerformanceEntry>> PerformanceObserver::takeRecords() {
  return {};
}
kj::Array<kj::String> PerformanceObserver::getSupportedEntryTypes() {
  // We return empty string because we don't support any of these entry types.
  // Because we return empty string, two of the web-platform tests fail. This is intentional.
  return kj::heapArray<kj::String>(0);
}

void Performance::eventLoopUtilization() {
  JSG_FAIL_REQUIRE(Error, "Performance.eventLoopUtilization is not implemented");
}

void Performance::markResourceTiming() {
  JSG_FAIL_REQUIRE(Error, "Performance.markResourceTiming is not implemented");
}

jsg::Function<void()> Performance::timerify(jsg::Lock& js, jsg::Function<void()> fn) {
  // We currently don't support timerify, so we just return the function as is.
  return kj::mv(fn);
}

jsg::Optional<uint32_t> EventCounts::get(kj::String eventType) {
  KJ_IF_SOME(count, eventCounts.find(eventType)) {
    return count;
  }
  return kj::none;
}

bool EventCounts::has(kj::String eventType) {
  return eventCounts.find(eventType) != kj::none;
}

uint32_t EventCounts::getSize() {
  return eventCounts.size();
}

jsg::Ref<EventCounts::EntryIterator> EventCounts::entries(jsg::Lock& js) {
  return jsg::alloc<EntryIterator>(IteratorState(JSG_THIS));
}

kj::Maybe<EventCounts::EntryIteratorType> EventCounts::entryIteratorNext(
    jsg::Lock& js, IteratorState& state) {
  if (state.index >= state.entries.size()) {
    return kj::none;
  }
  auto& entry = state.entries[state.index++];
  // Return [key, value] as an array
  return kj::arr(kj::str(kj::get<0>(entry)), kj::str(kj::get<1>(entry)));
}

jsg::Ref<EventCounts::KeyIterator> EventCounts::keys(jsg::Lock& js) {
  return jsg::alloc<KeyIterator>(IteratorState(JSG_THIS));
}

kj::Maybe<EventCounts::KeyIteratorType> EventCounts::keyIteratorNext(
    jsg::Lock& js, IteratorState& state) {
  if (state.index >= state.entries.size()) {
    return kj::none;
  }
  auto& entry = state.entries[state.index++];
  return kj::str(kj::get<0>(entry));
}

jsg::Ref<EventCounts::ValueIterator> EventCounts::values(jsg::Lock& js) {
  return jsg::alloc<ValueIterator>(IteratorState(JSG_THIS));
}

kj::Maybe<EventCounts::ValueIteratorType> EventCounts::valueIteratorNext(
    jsg::Lock& js, IteratorState& state) {
  if (state.index >= state.entries.size()) {
    return kj::none;
  }
  auto& entry = state.entries[state.index++];
  return kj::get<1>(entry);
}

void EventCounts::forEach(jsg::Lock& js,
    jsg::Function<void(uint32_t, kj::String, jsg::Ref<EventCounts>)> callback,
    jsg::Optional<jsg::JsValue> thisArg) {
  // Since we don't track events, this is effectively a no-op
  for (auto& entry: eventCounts) {
    callback(js, entry.value, kj::str(entry.key), JSG_THIS);
  }
}

jsg::Ref<EventCounts> Performance::getEventCounts(jsg::Lock& js) {
  // Return a new EventCounts instance (currently empty as we don't track events)
  return js.alloc<EventCounts>();
}

}  // namespace workerd::api
