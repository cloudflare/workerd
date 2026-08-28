// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
#include "basics.h"
#include "events.h"
#include "http.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

WD_STRONG_BOOL(AlreadyReported);

using kj::uint;
class Fetcher;
class ReadableStream;
class Response;

// Implements the web standard EventSource API
// https://developer.mozilla.org/en-US/docs/Web/API/EventSource
class EventSource: public EventTarget {
 public:
  struct EventSourceInit {
    // We don't actually make use of the standard withCredentials option. If this is set to
    // any truthy value, we'll throw.
    jsg::Optional<bool> withCredentials;

    // This is a non-standard workers-specific extension that allows the EventSource to
    // use a custom Fetcher instance.
    jsg::Optional<jsg::Ref<Fetcher>> fetcher;
    JSG_STRUCT(withCredentials, fetcher);
  };

  enum class State {
    CONNECTING = 0,
    OPEN = 1,
    CLOSED = 2,
  };

  EventSource(jsg::Lock& js, jsg::Url url, kj::Maybe<EventSourceInit> init = kj::none);

  EventSource(jsg::Lock& js);

  static jsg::Ref<EventSource> constructor(
      jsg::Lock& js, kj::String url, jsg::Optional<EventSourceInit> init);

  kj::ArrayPtr<const char> getUrl() const {
    KJ_IF_SOME(i, impl) {
      return i.url.getHref();
    }
    return nullptr;
  }
  bool getWithCredentials() const {
    return false;
  }
  uint getReadyState() const {
    return static_cast<uint>(readyState);
  }

  void close(jsg::Lock& js);

  // A non-standard extension that creates an EventSource instance around a ReadableStream
  // instance. In this instance, automatic reconnection is disabled since there is no URL
  // or underlying fetch used. The ReadableStream instance must produce bytes. It will be
  // locked and disturbed, and will be read until it either ends or errors. Calling close()
  // will cause the stream to be canceled.
  static jsg::Ref<EventSource> from(jsg::Lock& js, JsReadableStream stream);

  // The onopen, onmessage, and onerror event handler IDL attributes
  // (see EventTarget::setEventHandlerAttribute).
  kj::Maybe<jsg::JsValue> getOnOpen(jsg::Lock& js) {
    return getEventHandlerAttribute(js, "open"_kj);
  }
  void setOnOpen(
      jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler) {
    setEventHandlerAttribute(js, "open"_kj, kj::mv(handler));
  }
  kj::Maybe<jsg::JsValue> getOnMessage(jsg::Lock& js) {
    return getEventHandlerAttribute(js, "message"_kj);
  }
  void setOnMessage(
      jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler) {
    setEventHandlerAttribute(js, "message"_kj, kj::mv(handler));
  }
  kj::Maybe<jsg::JsValue> getOnError(jsg::Lock& js) {
    return getEventHandlerAttribute(js, "error"_kj);
  }
  void setOnError(
      jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler) {
    setEventHandlerAttribute(js, "error"_kj, kj::mv(handler));
  }

  JSG_RESOURCE_TYPE(EventSource) {
    JSG_INHERIT(EventTarget);
    JSG_METHOD(close);
    JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);
    JSG_READONLY_PROTOTYPE_PROPERTY(withCredentials, getWithCredentials);
    JSG_READONLY_PROTOTYPE_PROPERTY(readyState, getReadyState);
    JSG_PROTOTYPE_PROPERTY(onopen, getOnOpen, setOnOpen);
    JSG_PROTOTYPE_PROPERTY(onmessage, getOnMessage, setOnMessage);
    JSG_PROTOTYPE_PROPERTY(onerror, getOnError, setOnError);
    JSG_STATIC_CONSTANT_NAMED(CONNECTING, static_cast<uint>(State::CONNECTING));
    JSG_STATIC_CONSTANT_NAMED(OPEN, static_cast<uint>(State::OPEN));
    JSG_STATIC_CONSTANT_NAMED(CLOSED, static_cast<uint>(State::CLOSED));
    JSG_STATIC_METHOD(from);

    // EventSource is not defined by the spec as being disposable using ERM, but
    // it makes sense to do so. The dispose operation simply defers to close().
    // This will enable `using eventsource = new EventSource(...)`
    JSG_DISPOSE(close);
  }

  struct PendingMessage {
    kj::Vector<kj::String> data;
    kj::Maybe<kj::String> event;
    kj::String id;
  };

  // Called by the internal implementation to notify the EventSource about messages
  // received from the server.
  void enqueueMessages(
      kj::Array<PendingMessage> messages, kj::Rc<jsg::WeakRef<EventSource>> weakSelf);

  // Called by the internal implementation to notify the EventSource that the server
  // has provided a new reconnection time.
  void setReconnectionTime(uint32_t time);

  // Called by the internal implementation to retrieve the last event id that was
  // specified by the server.
  kj::StringPtr getLastEventId();

  // Called by the internal implementation to set the last event id that was specified
  // by the server.
  void setLastEventId(kj::String id);

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  IoContext& context;
  struct FetchImpl {
    jsg::Url url;
    EventSourceInit options;
    // Indicates that the server previously responded with no content after a
    // successful connection. This is likely indicative of a bug on the server.
    // If this happens once, we'll try to reconnect. If it happens again, we'll
    // fail the connection.
    bool previousNoBody = false;
  };
  // Used when the EventSource is created using the constructor. This
  // is the normal mode of operation, when the EventSource uses fetch
  // under the covers to connect, and reconnect, to the server. This
  // will be kj::none when the EventSource is created using the from()
  // method.
  kj::Maybe<FetchImpl> impl;
  jsg::Ref<AbortController> abortController;
  State readyState;
  kj::String lastEventId;

  // Indicates that the close method has been previously called.
  bool closeCalled = false;

  // The EventSource spec defines onopen, onmessage, and onerror as prototype
  // properties on the class.

  // The default reconnection wait time. This is fairly arbitrary and is left
  // entirely up to the implementation. The event stream can provide a new value.
  static constexpr auto DEFAULT_RECONNECTION_TIME = 2 * kj::SECONDS;
  static constexpr uint32_t MIN_RECONNECTION_TIME = 1000;
  static constexpr uint32_t MAX_RECONNECTION_TIME = 10 * 1000;

  kj::Duration reconnectionTime = DEFAULT_RECONNECTION_TIME;

  void notifyOpen(jsg::Lock& js);
  // AlreadyReported::YES indicates the error was already delivered to the global scope's
  // report-an-exception machinery (a reported listener exception), so notifyError() must
  // not log it again.
  void notifyError(jsg::Lock& js,
      const jsg::JsValue& error,
      bool reconnecting = false,
      AlreadyReported alreadyReported = AlreadyReported::NO);
  void notifyMessages(jsg::Lock& js, kj::Array<PendingMessage> messages);

  // The run() method handles the actual processing of the stream.
  void run(jsg::Lock& js,
      JsReadableStream stream,
      bool withReconnection = true,
      kj::Maybe<jsg::Ref<Response>> response = kj::none,
      kj::Maybe<jsg::Ref<Fetcher>> fetcher = kj::none);
  // The start() method initializes the fetch and the processing of the
  // stream by calling run.
  void start(jsg::Lock& js);
  void reconnect(jsg::Lock& js);
};

}  // namespace workerd::api

#define EW_EVENTSOURCE_ISOLATE_TYPES api::EventSource, api::EventSource::EventSourceInit
