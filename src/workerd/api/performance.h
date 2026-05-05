// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>
#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {
class IsolateLimitEnforcer;
};

namespace workerd::api {

// ======================================================================================
// Performance API
// ======================================================================================
//
// This implementation provides a subset of the Performance API for compatibility with
// other JavaScript runtimes. We are not intending to fully implement in-worker
// performance-timing feedback as Cloudflare Workers run in a different context than
// traditional browser or Node.js environments.
//
// The APIs here are primarily provided to support code portability and to prevent
// runtime errors when code expects these standard APIs to exist.
//
// Specifications:
// - W3C Performance Timeline: https://w3c.github.io/performance-timeline/
// - W3C User Timing: https://w3c.github.io/user-timing/
// - MDN Documentation: https://developer.mozilla.org/en-US/docs/Web/API/Performance_API
//
// Current limitations:
// - No actual performance metrics collection within workers
// - PerformanceObserver is provided but with minimal functionality
// - Most entry types are not supported
// - Timing data may not reflect actual worker execution characteristics

class PerformanceEntry: public jsg::Object {
 public:
  PerformanceEntry(
      kj::String name, kj::LiteralStringConst entryType, double startTime, double duration)
      : name(kj::mv(name)),
        entryType(kj::mv(entryType)),
        startTime(startTime),
        duration(duration) {}

  kj::StringPtr getName() {
    return name;
  };
  kj::StringPtr getEntryType() {
    return entryType;
  };
  double getStartTime() {
    return startTime;
  };
  double getDuration() {
    return duration;
  };

  jsg::JsObject toJSON(jsg::Lock& js);

  JSG_RESOURCE_TYPE(PerformanceEntry) {
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
    JSG_READONLY_PROTOTYPE_PROPERTY(entryType, getEntryType);
    JSG_READONLY_PROTOTYPE_PROPERTY(startTime, getStartTime);
    JSG_READONLY_PROTOTYPE_PROPERTY(duration, getDuration);
    JSG_METHOD(toJSON);

    JSG_TS_OVERRIDE({
      toJSON(): object;
    });
  }

 protected:
  kj::String name;
  kj::LiteralStringConst entryType;
  double startTime;
  double duration;
};

class PerformanceMark: public PerformanceEntry {
 public:
  friend class Performance;

  struct Options {
    jsg::Optional<jsg::JsRef<jsg::JsObject>> detail;
    jsg::Optional<double> startTime;

    JSG_STRUCT(detail, startTime);
  };

  PerformanceMark(
      kj::String name, jsg::Optional<jsg::JsRef<jsg::JsObject>> detail, double startTime)
      : PerformanceEntry(kj::mv(name), "mark"_kjc, startTime, 0),
        detail(kj::mv(detail)) {}

  static jsg::Ref<PerformanceMark> constructor(
      jsg::Lock& js, kj::String name, jsg::Optional<Options> maybeOptions);

  jsg::JsValue getDetail(jsg::Lock& js) {
    KJ_IF_SOME(d, detail) {
      return d.getHandle(js);
    }
    return js.null();
  }

  jsg::JsObject toJSON(jsg::Lock& js);

  JSG_RESOURCE_TYPE(PerformanceMark) {
    JSG_INHERIT(PerformanceEntry);
    JSG_READONLY_PROTOTYPE_PROPERTY(detail, getDetail);
    JSG_METHOD(toJSON);

    JSG_TS_OVERRIDE({
      toJSON(): object;
    });
  }

 private:
  jsg::Optional<jsg::JsRef<jsg::JsObject>> detail;
};

// UvMetricsInfo represents libuv event loop metrics.
// In workerd, this returns stub values since actual libuv metrics are not available.
struct UvMetricsInfo {
  double loopCount;
  double events;
  double eventsWaiting;

  JSG_STRUCT(loopCount, events, eventsWaiting);
};

// PerformanceNodeTiming provides Node.js-specific timing information.
// In workerd, this returns stub values since actual Node.js startup metrics are not applicable.
// This class is only exposed when the Node.js perf_hooks compat flag is enabled.
//
// Spec: https://nodejs.org/api/perf_hooks.html#class-performancenodetiming
class PerformanceNodeTiming: public PerformanceEntry {
 public:
  PerformanceNodeTiming(): PerformanceEntry(kj::str("node"), "node"_kjc, 0, 0) {}

  static jsg::Ref<PerformanceNodeTiming> constructor() = delete;

  // All timing values return 0 as stubs since Node.js startup metrics
  // are not applicable in the Workers context.
  double getNodeStart() {
    return 0;
  }
  double getV8Start() {
    return 0;
  }
  double getBootstrapComplete() {
    return 0;
  }
  double getEnvironment() {
    return 0;
  }
  double getLoopStart() {
    return 0;
  }
  double getLoopExit() {
    return 0;
  }
  double getIdleTime() {
    return 0;
  }

  // uvMetricsInfo returns libuv event loop metrics.
  // In workerd, this returns stub values since actual libuv metrics are not available.
  // Spec: https://nodejs.org/api/perf_hooks.html#performancenodetiminguvmetricsinfo
  UvMetricsInfo getUvMetricsInfo(jsg::Lock& js) {
    return UvMetricsInfo{
      .loopCount = 0,
      .events = 0,
      .eventsWaiting = 0,
    };
  }

  jsg::JsObject toJSON(jsg::Lock& js);

  // Node.js exposes these as instance properties (own properties on the object),
  // not prototype properties. This matches Node.js behavior where:
  //   Reflect.ownKeys(performance.nodeTiming) includes all these properties
  //   Reflect.ownKeys(performance.nodeTiming.__proto__) only has constructor, toJSON
  JSG_RESOURCE_TYPE(PerformanceNodeTiming) {
    JSG_INHERIT(PerformanceEntry);
    JSG_READONLY_INSTANCE_PROPERTY(nodeStart, getNodeStart);
    JSG_READONLY_INSTANCE_PROPERTY(v8Start, getV8Start);
    JSG_READONLY_INSTANCE_PROPERTY(bootstrapComplete, getBootstrapComplete);
    JSG_READONLY_INSTANCE_PROPERTY(environment, getEnvironment);
    JSG_READONLY_INSTANCE_PROPERTY(loopStart, getLoopStart);
    JSG_READONLY_INSTANCE_PROPERTY(loopExit, getLoopExit);
    JSG_READONLY_INSTANCE_PROPERTY(idleTime, getIdleTime);
    JSG_READONLY_INSTANCE_PROPERTY(uvMetricsInfo, getUvMetricsInfo);
    JSG_METHOD(toJSON);

    JSG_TS_OVERRIDE({
      toJSON(): object;
    });
  }
};

class PerformanceMeasure: public PerformanceEntry {
 public:
  PerformanceMeasure(kj::String name, double startTime, double duration)
      : PerformanceEntry(kj::mv(name), "measure"_kjc, startTime, duration) {}

  friend class Performance;

  struct Entry {
    kj::String entryType;
    kj::String name;
    kj::OneOf<kj::Date, double> startTime;
    double duration;
    jsg::Optional<jsg::JsRef<jsg::JsObject>> detail;

    JSG_STRUCT(entryType, name, startTime, duration, detail);
  };

  struct Options {
    jsg::Optional<jsg::JsRef<jsg::JsObject>> detail;
    jsg::Optional<double> start;
    jsg::Optional<double> duration;
    jsg::Optional<double> end;

    JSG_STRUCT(detail, start, duration, end);
  };

  jsg::JsValue getDetail(jsg::Lock& js) {
    KJ_IF_SOME(d, detail) {
      return d.getHandle(js);
    }
    return js.null();
  }

  jsg::JsObject toJSON(jsg::Lock& js);

  JSG_RESOURCE_TYPE(PerformanceMeasure) {
    JSG_INHERIT(PerformanceEntry);
    JSG_READONLY_PROTOTYPE_PROPERTY(detail, getDetail);
    JSG_METHOD(toJSON);

    JSG_TS_OVERRIDE({
      toJSON(): object;
    });
  }

 private:
  jsg::Optional<jsg::JsRef<jsg::JsObject>> detail;
};

class PerformanceResourceTiming: public PerformanceEntry {
 public:
  PerformanceResourceTiming(kj::String name, double startTime, double duration)
      : PerformanceEntry(kj::mv(name), "resource"_kjc, startTime, duration) {}

  uint32_t getConnectEnd() {
    return 0;
  }
  uint32_t getConnectStart() {
    return 0;
  }
  uint32_t getDecodedBodySize() {
    return 0;
  }
  uint32_t getDomainLookupEnd() {
    return 0;
  }
  uint32_t getDomainLookupStart() {
    return 0;
  }
  uint32_t getEncodedBodySize() {
    return 0;
  }
  uint32_t getFetchStart() {
    return 0;
  }
  jsg::JsString getInitiatorType(jsg::Lock& js) {
    return js.str();
  }
  jsg::JsString getNextHopProtocol(jsg::Lock& js) {
    return js.str();
  }
  uint32_t getRedirectEnd() {
    return 0;
  }
  uint32_t getRedirectStart() {
    return 0;
  }
  uint32_t getRequestStart() {
    return 0;
  }
  uint32_t getResponseEnd() {
    return 0;
  }
  uint32_t getResponseStart() {
    return 0;
  }
  uint32_t getResponseStatus() {
    return 0;
  }
  jsg::Optional<uint32_t> getSecureConnectionStart() {
    return kj::none;
  }
  uint32_t getTransferSize() {
    return 0;
  }
  uint32_t getWorkerStart() {
    return 0;
  }

  jsg::JsObject toJSON(jsg::Lock& js);

  JSG_RESOURCE_TYPE(PerformanceResourceTiming) {
    JSG_INHERIT(PerformanceEntry);
    JSG_READONLY_PROTOTYPE_PROPERTY(connectEnd, getConnectEnd);
    JSG_READONLY_PROTOTYPE_PROPERTY(connectStart, getConnectStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(decodedBodySize, getDecodedBodySize);
    JSG_READONLY_PROTOTYPE_PROPERTY(domainLookupEnd, getDomainLookupEnd);
    JSG_READONLY_PROTOTYPE_PROPERTY(domainLookupStart, getDomainLookupStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(encodedBodySize, getEncodedBodySize);
    JSG_READONLY_PROTOTYPE_PROPERTY(fetchStart, getFetchStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(initiatorType, getInitiatorType);
    JSG_READONLY_PROTOTYPE_PROPERTY(nextHopProtocol, getNextHopProtocol);
    JSG_READONLY_PROTOTYPE_PROPERTY(redirectEnd, getRedirectEnd);
    JSG_READONLY_PROTOTYPE_PROPERTY(redirectStart, getRedirectStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(requestStart, getRequestStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(responseEnd, getResponseEnd);
    JSG_READONLY_PROTOTYPE_PROPERTY(responseStart, getResponseStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(responseStatus, getResponseStatus);
    JSG_READONLY_PROTOTYPE_PROPERTY(secureConnectionStart, getSecureConnectionStart);
    JSG_READONLY_PROTOTYPE_PROPERTY(transferSize, getTransferSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(workerStart, getWorkerStart);
  }
};

class PerformanceObserverEntryList: public jsg::Object {
 public:
  kj::ArrayPtr<jsg::Ref<PerformanceEntry>> getEntries();
  kj::ArrayPtr<jsg::Ref<PerformanceEntry>> getEntriesByType(kj::String type);
  kj::ArrayPtr<jsg::Ref<PerformanceEntry>> getEntriesByName(
      kj::String name, jsg::Optional<kj::String> type);

  void visitForGc(jsg::GcVisitor& visitor) {
    // No managed objects to visit currently
  }

  JSG_RESOURCE_TYPE(PerformanceObserverEntryList) {
    JSG_METHOD(getEntries);
    JSG_METHOD(getEntriesByType);
    JSG_METHOD(getEntriesByName);
  }
};

// PerformanceObserver provides a way to observe performance timeline entries.
// This is a minimal implementation for compatibility purposes.
//
// Spec: https://w3c.github.io/performance-timeline/#the-performanceobserver-interface
// MDN: https://developer.mozilla.org/en-US/docs/Web/API/PerformanceObserver
//
// Note: In the Workers environment, this observer will not receive most performance
// entries as we don't track detailed performance metrics within workers. The API
// is provided mainly for compatibility with code that expects it to exist.
class PerformanceObserver: public jsg::Object {
 public:
  struct CallbackOptions {
    jsg::Optional<uint32_t> droppedEntriesCount;
    JSG_STRUCT(droppedEntriesCount);
  };

  using Callback = jsg::JsValue;

  static jsg::Ref<PerformanceObserver> constructor(jsg::Lock& js, Callback callback);

  PerformanceObserver(jsg::Lock& js, Callback callback): callback(callback.addRef(js)) {}

  struct ObserveOptions {
    jsg::Optional<bool> buffered;
    jsg::Optional<uint32_t> durationThreshold = 104;
    jsg::Optional<kj::Array<kj::String>> entryTypes;
    jsg::Optional<kj::String> type;

    JSG_STRUCT(buffered, durationThreshold, entryTypes, type);
  };
  void disconnect();
  void observe(jsg::Optional<ObserveOptions> options);
  kj::Array<jsg::Ref<PerformanceEntry>> takeRecords();
  // Spec: https://w3c.github.io/performance-timeline/#supportedentrytypes-attribute
  static kj::ArrayPtr<const kj::StringPtr> getSupportedEntryTypes();

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(callback);
  }

  JSG_RESOURCE_TYPE(PerformanceObserver) {
    JSG_METHOD(disconnect);
    JSG_METHOD(observe);
    JSG_METHOD(takeRecords);
    JSG_STATIC_READONLY_PROPERTY_NAMED(supportedEntryTypes, getSupportedEntryTypes);
  }

 private:
  jsg::JsRef<jsg::JsValue> callback;

  static constexpr kj::FixedArray<kj::StringPtr, 2> supportedEntryTypes = []() consteval {
    // We have mark and measure as supported because we implement relevant methods.
    kj::FixedArray<kj::StringPtr, 2> out;
    out[0] = "measure"_kj;
    out[1] = "mark"_kj;
    return kj::mv(out);
  }();
};

// EventCounts provides a read-only map of event counts per event type.
// This is a minimal implementation for compatibility with the EventCounts API.
//
// Spec: https://w3c.github.io/event-timing/#eventcounts
// MDN: https://developer.mozilla.org/en-US/docs/Web/API/EventCounts
//
// The EventCounts interface is a read-only map-like object where:
// - Keys are event type strings (e.g., "click", "keydown")
// - Values are the number of events dispatched for that type
// - It doesn't have clear(), delete(), or set() methods
class EventCounts: public jsg::Object {
 public:
  EventCounts() = default;

  jsg::Optional<uint32_t> get(kj::String eventType);
  bool has(kj::String eventType);
  uint32_t getSize();

  struct IteratorState {
    jsg::Ref<EventCounts> parent;
    size_t index = 0;
    kj::Vector<kj::Tuple<kj::String, uint32_t>> entries;

    IteratorState(jsg::Ref<EventCounts> parent): parent(kj::mv(parent)) {
      // Copy the entries from the map into our vector for stable iteration
      // Check if the map has any entries before iterating
      if (this->parent->eventCounts.size() > 0) {
        for (auto& entry: this->parent->eventCounts) {
          entries.add(kj::tuple(kj::str(entry.key), entry.value));
        }
      }
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }
  };

  using EntryIteratorType = kj::Array<kj::String>;
  using KeyIteratorType = kj::String;
  using ValueIteratorType = uint32_t;

  JSG_ITERATOR(EntryIterator, entries, EntryIteratorType, IteratorState, entryIteratorNext)
  JSG_ITERATOR(KeyIterator, keys, KeyIteratorType, IteratorState, keyIteratorNext)
  JSG_ITERATOR(ValueIterator, values, ValueIteratorType, IteratorState, valueIteratorNext)

  void forEach(jsg::Lock& js,
      jsg::Function<void(uint32_t, kj::String, jsg::Ref<EventCounts>)>,
      jsg::Optional<jsg::JsValue>);

  void visitForGc(jsg::GcVisitor& visitor) {
    // No managed objects to visit currently
  }

  JSG_RESOURCE_TYPE(EventCounts) {
    JSG_READONLY_PROTOTYPE_PROPERTY(size, getSize);
    JSG_METHOD(get);
    JSG_METHOD(has);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_METHOD(forEach);
    JSG_ITERABLE(entries);
  }

 private:
  // Static iterator next methods
  static kj::Maybe<EntryIteratorType> entryIteratorNext(jsg::Lock& js, IteratorState& state);
  static kj::Maybe<KeyIteratorType> keyIteratorNext(jsg::Lock& js, IteratorState& state);
  static kj::Maybe<ValueIteratorType> valueIteratorNext(jsg::Lock& js, IteratorState& state);

  // For now, we keep this empty as we don't actually track events in the worker context.
  // This can be extended in the future to store actual event counts.
  kj::HashMap<kj::String, uint32_t> eventCounts;

  friend struct IteratorState;
};

// Performance provides timing-related functionality and performance metrics.
// This is a minimal implementation focused on compatibility rather than providing
// detailed performance insights within the Workers environment.
//
// Spec: https://w3c.github.io/hr-time/#the-performance-interface
// MDN: https://developer.mozilla.org/en-US/docs/Web/API/Performance
//
// Key limitations in Workers:
// - performance.now() returns the same precision as Date.now() for security reasons
// - Most performance entry types are not supported
// - Resource timing and navigation timing are not applicable in the Workers context
// - User timing (marks and measures) have limited implementation
class Performance: public EventTarget {
 public:
  static jsg::Ref<Performance> constructor() = delete;

  explicit Performance(const IsolateLimitEnforcer& isolateLimitEnforcer)
      : isolateLimitEnforcer(isolateLimitEnforcer) {}

  // We always return a time origin of 0, making performance.now() equivalent to Date.now(). There
  // is no other appropriate time origin to use given that the Worker platform is intended to be
  // treated like one big computer rather than many individual instances. In particular, if and
  // when we start snapshotting applications after startup and then starting instances from that
  // snapshot, what would the right time origin be? The time when the snapshot was created? This
  // seems to leak implementation details in a weird way.
  //
  // Note that the purpose of `timeOrigin` is normally to allow `now()` to return a more-precise
  // measurement. Measuring against a recent time allows the values returned by `now()` to be
  // smaller in magnitude, which alzlows them to be more precise due to the nature of floating
  // point numbers. In our case, though, we don't return precise measurements from this interface
  // anyway, for Spectre reasons -- it returns the same as Date.now().
  double getTimeOrigin() {
    return 0.0;
  }

  jsg::Ref<EventCounts> getEventCounts(jsg::Lock& js);

  double now(jsg::Lock& js);

  void clearMarks(jsg::Optional<kj::String> name);
  void clearMeasures(jsg::Optional<kj::String> name);
  void clearResourceTimings();
  kj::ArrayPtr<jsg::Ref<PerformanceEntry>> getEntries();
  kj::Array<jsg::Ref<PerformanceEntry>> getEntriesByName(
      kj::String name, jsg::Optional<kj::String> type);
  kj::Array<jsg::Ref<PerformanceEntry>> getEntriesByType(kj::String type);
  jsg::Ref<PerformanceMark> mark(
      jsg::Lock& js, kj::String name, jsg::Optional<PerformanceMark::Options> options);

  // Following signatures are supported:
  // - measure(measureName)
  // - measure(measureName, startMark)
  // - measure(measureName, startMark, endMark)
  // - measure(measureName, measureOptions)
  // - measure(measureName, measureOptions, endMark)
  jsg::Ref<PerformanceMeasure> measure(jsg::Lock& js,
      kj::String measureName,
      jsg::Optional<kj::OneOf<PerformanceMeasure::Options, kj::String>> measureOptionsOrStartMark =
          kj::none,
      jsg::Optional<kj::String> maybeEndMark = kj::none);

  void setResourceTimingBufferSize(uint32_t size);

  jsg::JsObject toJSON(jsg::Lock& js);

  // Node.js-specific performance extensions.
  // These are provided as stubs for compatibility with code that expects Node.js APIs.

  // EventLoopUtilization represents the utilization of the event loop.
  // In workerd, we return stub values since actual event loop metrics are not available.
  struct EventLoopUtilization {
    double idle = 0;
    double active = 0;
    double utilization = 0;

    JSG_STRUCT(idle, active, utilization);
  };

  EventLoopUtilization eventLoopUtilization();

  // Returns the PerformanceNodeTiming object containing Node.js-specific timing metrics.
  // In workerd, this returns stub values since Node.js startup metrics are not applicable.
  jsg::Ref<PerformanceNodeTiming> getNodeTiming(jsg::Lock& js);

  // In the browser, this function is not public. However, it must be used inside fetch
  // which is a Node.js dependency, not an internal module.
  // Returns void as a no-op stub since resource timing is not applicable in Workers.
  void markResourceTiming();
  jsg::Function<void()> timerify(jsg::Lock& js, jsg::Function<void()> fn);

  JSG_RESOURCE_TYPE(Performance, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(timeOrigin, getTimeOrigin);
    JSG_METHOD(now);

    // The following are provided as non-ops to ensure availability
    // of the APIS but we are currently not planning to provide
    // performance timing feedback within a worker using these
    // apis.
    if (flags.getEnableGlobalPerformanceClasses() || flags.getEnableNodeJsPerfHooksModule()) {
      JSG_INHERIT(EventTarget);
      JSG_READONLY_PROTOTYPE_PROPERTY(eventCounts, getEventCounts);
      JSG_METHOD(clearMarks);
      JSG_METHOD(clearMeasures);
      JSG_METHOD(clearResourceTimings);
      JSG_METHOD(getEntries);
      JSG_METHOD(getEntriesByName);
      JSG_METHOD(getEntriesByType);
      JSG_METHOD(mark);
      JSG_METHOD(measure);
      JSG_METHOD(setResourceTimingBufferSize);
      JSG_METHOD(toJSON);
    }

    if (flags.getEnableNodeJsPerfHooksModule()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(nodeTiming, getNodeTiming);
      JSG_METHOD(eventLoopUtilization);
      JSG_METHOD(markResourceTiming);
      JSG_METHOD(timerify);
    }

    JSG_TS_OVERRIDE({
      toJSON(): object;
    });
  }

 private:
  const IsolateLimitEnforcer& isolateLimitEnforcer;
  kj::Vector<jsg::Ref<PerformanceEntry>> entries;
};

#define EW_PERFORMANCE_ISOLATE_TYPES                                                               \
  api::Performance, api::Performance::EventLoopUtilization, api::PerformanceNodeTiming,            \
      api::UvMetricsInfo, api::PerformanceMark, api::PerformanceMeasure,                           \
      api::PerformanceMark::Options, api::PerformanceMeasure::Options,                             \
      api::PerformanceMeasure::Entry, api::PerformanceObserverEntryList, api::PerformanceEntry,    \
      api::PerformanceResourceTiming, api::PerformanceObserver,                                    \
      api::PerformanceObserver::ObserveOptions, api::PerformanceObserver::CallbackOptions,         \
      api::EventCounts, api::EventCounts::EntryIterator, api::EventCounts::EntryIterator::Next,    \
      api::EventCounts::KeyIterator, api::EventCounts::KeyIterator::Next,                          \
      api::EventCounts::ValueIterator, api::EventCounts::ValueIterator::Next

}  // namespace workerd::api
