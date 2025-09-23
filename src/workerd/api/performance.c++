// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "performance.h"

#include <workerd/io/io-util.h>
#include <workerd/io/worker.h>

#include <kj/encoding.h>

namespace workerd::api {

double Performance::now(jsg::Lock& js) {
  // We define performance.now() for compatibility purposes, but due to Spectre concerns it
  // returns exactly what Date.now() returns.
  isolateLimitEnforcer.markPerfEvent("performance_now"_kjc);
  return dateNow();
}

jsg::Ref<PerformanceMark> PerformanceMark::constructor(
    jsg::Lock& js, kj::String name, jsg::Optional<Options> maybeOptions) {
  auto options = kj::mv(maybeOptions).orDefault({});
  return js.alloc<PerformanceMark>(
      kj::mv(name), kj::mv(options.detail), options.startTime.orDefault(dateNow()));
}

jsg::JsObject PerformanceMark::toJSON(jsg::Lock& js) {
  auto obj = js.objNoProto();
  obj.set(js, "name"_kj, js.str(name));
  obj.set(js, "entryType"_kj, js.str(entryType));
  obj.set(js, "startTime"_kj, js.num(startTime));
  obj.set(js, "duration"_kj, js.num(duration));
  KJ_IF_SOME(d, getDetail(js)) {
    obj.set(js, "detail"_kj, d);
  }
  return kj::mv(obj);
}

jsg::JsObject PerformanceMeasure::toJSON(jsg::Lock& js) {
  auto obj = js.objNoProto();
  obj.set(js, "name"_kj, js.str(name));
  obj.set(js, "entryType"_kj, js.str(entryType));
  obj.set(js, "startTime"_kj, js.num(startTime));
  obj.set(js, "duration"_kj, js.num(duration));
  KJ_IF_SOME(d, getDetail(js)) {
    obj.set(js, "detail"_kj, d);
  }
  return kj::mv(obj);
}

jsg::JsObject PerformanceEntry::toJSON(jsg::Lock& js) {
  auto obj = js.objNoProto();
  obj.set(js, "name"_kj, js.str(name));
  obj.set(js, "entryType"_kj, js.str(entryType));
  obj.set(js, "startTime"_kj, js.num(startTime));
  obj.set(js, "duration"_kj, js.num(duration));
  return kj::mv(obj);
}

jsg::JsObject PerformanceResourceTiming::toJSON(jsg::Lock& js) {
  JSG_FAIL_REQUIRE(Error, "PerformanceResourceTiming.toJSON is not implemented"_kj);
}

void Performance::clearMarks(jsg::Optional<kj::String> name) {
  kj::Vector<jsg::Ref<PerformanceEntry>> filtered;

  KJ_IF_SOME(n, name) {
    for (auto& entry: entries) {
      if (entry->getName() != n) {
        filtered.add(kj::mv(entry));
      }
    }
  } else {
    for (auto& entry: entries) {
      if (entry->getEntryType() != "mark") {
        filtered.add(kj::mv(entry));
      }
    }
  }

  entries = filtered.releaseAsArray();
}

void Performance::clearMeasures(jsg::Optional<kj::String> name) {
  kj::Vector<jsg::Ref<PerformanceEntry>> filtered;

  KJ_IF_SOME(n, name) {
    for (auto& entry: entries) {
      if (entry->getName() != n) {
        filtered.add(kj::mv(entry));
      }
    }
  } else {
    for (auto& entry: entries) {
      if (entry->getEntryType() != "measure") {
        filtered.add(kj::mv(entry));
      }
    }
  }

  entries = filtered.releaseAsArray();
}

void Performance::clearResourceTimings() {
  kj::Vector<jsg::Ref<PerformanceEntry>> filtered;

  // Remove entries where entryType is "resource" or "navigation"
  for (auto& entry: entries) {
    auto entryType = entry->getEntryType();
    if (entryType != "resource"_kj && entryType != "navigation"_kj) {
      filtered.add(kj::mv(entry));
    }
  }

  entries = filtered.releaseAsArray();
}

kj::ArrayPtr<jsg::Ref<PerformanceEntry>> Performance::getEntries() {
  return entries;
}

kj::Array<jsg::Ref<PerformanceEntry>> Performance::getEntriesByName(
    kj::String name, jsg::Optional<kj::String> type) {
  kj::Vector<jsg::Ref<PerformanceEntry>> filtered;

  for (auto& entry: entries) {
    if (entry->getName() == name) {
      KJ_IF_SOME(t, type) {
        if (entry->getEntryType() == t) {
          filtered.add(entry.addRef());
        }
      } else {
        filtered.add(entry.addRef());
      }
    }
  }

  return filtered.releaseAsArray();
}

kj::Array<jsg::Ref<PerformanceEntry>> Performance::getEntriesByType(kj::String type) {
  kj::Vector<jsg::Ref<PerformanceEntry>> filtered;

  for (auto& entry: entries) {
    if (entry->getEntryType() == type) {
      filtered.add(entry.addRef());
    }
  }

  return filtered.releaseAsArray();
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
  // TODO(someday): Include `name` in the perf event name?
  isolateLimitEnforcer.markPerfEvent("performance_mark"_kjc);
  double startTime = dateNow();
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(time, opts.startTime) {
      startTime = time;
    }
  }

  auto mark = js.alloc<PerformanceMark>(kj::mv(name), kj::none, startTime);

  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(d, opts.detail) {
      mark->detail = kj::mv(d);
    }
  }

  entries.add(mark.addRef());
  return mark;
}

jsg::Ref<PerformanceMeasure> Performance::measure(jsg::Lock& js,
    kj::String measureName,
    kj::OneOf<PerformanceMeasure::Options, kj::String> measureOptionsOrStartMark,
    jsg::Optional<kj::String> maybeEndMark) {
  isolateLimitEnforcer.markPerfEvent("performance_measure"_kjc);
  double startTime = dateNow();
  double endTime = startTime;

  KJ_SWITCH_ONEOF(measureOptionsOrStartMark) {
    KJ_CASE_ONEOF(startMark, kj::String) {
      auto startMarks = getEntriesByName(kj::str(startMark), kj::str("mark"));
      if (startMarks.size() > 0) {
        startTime = startMarks[0]->getStartTime();
      }

      KJ_IF_SOME(endMark, maybeEndMark) {
        auto endMarks = getEntriesByName(kj::str(endMark), kj::str("mark"));
        if (endMarks.size() > 0) {
          endTime = endMarks[0]->getStartTime();
        }
      }
    }
    KJ_CASE_ONEOF(options, PerformanceMeasure::Options) {
      KJ_IF_SOME(start, options.start) {
        startTime = start;
      }

      KJ_IF_SOME(end, options.end) {
        endTime = end;
      } else KJ_IF_SOME(duration, options.duration) {
        endTime = startTime + duration;
      }
    }
  }

  uint32_t duration = endTime >= startTime ? endTime - startTime : 0;
  auto measure = js.alloc<PerformanceMeasure>(kj::mv(measureName), startTime, duration);

  KJ_SWITCH_ONEOF(measureOptionsOrStartMark) {
    KJ_CASE_ONEOF(startMark, kj::String) {
      auto detailObj = js.objNoProto();
      detailObj.set(js, "start", js.num(startTime));
      detailObj.set(js, "end", js.num(endTime));
      measure->detail = jsg::JsRef<jsg::JsObject>(js, detailObj);
    }
    KJ_CASE_ONEOF(options, PerformanceMeasure::Options) {
      KJ_IF_SOME(d, options.detail) {
        measure->detail = jsg::JsRef<jsg::JsObject>(js, d.getHandle(js));
      } else {
        auto detailObj = js.objNoProto();
        detailObj.set(js, "start", js.num(startTime));
        detailObj.set(js, "end", js.num(endTime));
        measure->detail = jsg::JsRef<jsg::JsObject>(js, detailObj);
      }
    }
  }

  entries.add(measure.addRef());
  return measure;
}

void Performance::setResourceTimingBufferSize(uint32_t size) {
  JSG_FAIL_REQUIRE(Error, "Performance.setResourceTimingBufferSize is not implemented");
}

jsg::Ref<PerformanceObserver> PerformanceObserver::constructor(jsg::Lock& js, Callback callback) {
  return js.alloc<PerformanceObserver>(js, callback);
}

void PerformanceObserver::disconnect() {
  // Leaving it as a no-op for now.
}

void PerformanceObserver::observe(jsg::Optional<ObserveOptions> options) {
  // Leaving it as a no-op for now.
}

kj::Array<jsg::Ref<PerformanceEntry>> PerformanceObserver::takeRecords() {
  return {};
}
kj::ArrayPtr<const kj::StringPtr> PerformanceObserver::getSupportedEntryTypes() {
  return supportedEntryTypes.asPtr();
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
