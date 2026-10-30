// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

WD_STRONG_BOOL(ReportListenerExceptions);

// The C++ half of the seam through which EventTarget subclasses implemented in the per-isolate
// bootstrap (src/per_isolate/) reach the C++ EventTarget they extend: the parts of EventTarget
// that C++ subclasses use but that are not exposed to JavaScript. The bootstrap exposes these as
// `utils` members, which bootstrap code reaches only through
// src/per_isolate/events/event-target-internals.ts; that module documents the contract.
//
// Every function throws a TypeError if `target` is not an EventTarget or `type` is not a string.
class EventTargetBootstrap {
 public:
  // Registers `hook` to be called as hook(type, count), with an undefined receiver, whenever the
  // number of listeners registered on `target` for `type` changes. What counts is described at
  // EventTarget::listenerCountChanged(). Replaces any hook previously registered on `target` for
  // the same type. The hook runs synchronously inside addEventListener(), removeEventListener()
  // and dispatch, so it must neither run user code nor throw.
  static void setListenerCountHook(
      jsg::Lock& js, jsg::JsValue target, jsg::JsValue type, jsg::JsValue hook);

  // The on<type> event handler IDL attribute; see EventTarget::setEventHandlerAttribute(). The
  // getter returns null when the attribute holds no handler. A callable handler is invoked with
  // the global object as its receiver, as JSG invokes one assigned through a C++ setter.
  static jsg::JsValue getEventHandlerAttribute(
      jsg::Lock& js, jsg::JsValue target, jsg::JsValue type);
  static void setEventHandlerAttribute(
      jsg::Lock& js, jsg::JsValue target, jsg::JsValue type, jsg::JsValue value);

  // Dispatches `event` (an Event) at `target`. The exception policy is applied as given, not
  // through EventTarget::effectiveExceptionPolicy(). When reporting, returns undefined if no
  // listener threw, else a null-prototype record whose `exception` member is the first listener
  // exception. Otherwise the first listener exception ends the dispatch and propagates, and the
  // result is undefined.
  static jsg::JsValue dispatchEvent(jsg::Lock& js,
      jsg::JsValue target,
      jsg::JsValue event,
      ReportListenerExceptions reportListenerExceptions);

  // Creates a trusted MessageEvent of the given type, as the runtime fires one: `data` as is, an
  // empty lastEventId, a null origin, no ports, and `source` (a MessagePort, or null or
  // undefined for none). Throws a TypeError if `type` is not a string or `source` is neither.
  static jsg::JsValue createTrustedMessageEvent(
      jsg::Lock& js, jsg::JsValue type, jsg::JsValue data, jsg::JsValue source);
};

}  // namespace workerd::api
