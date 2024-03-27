#pragma once
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
#include "basics.h"

namespace workerd::api {

using kj::uint;
class Fetcher;
class ReadableStream;
class Response;

// Implements the web standard EventSource API
// https://developer.mozilla.org/en-US/docs/Web/API/EventSource
class EventSource: public EventTarget {
public:
  class ErrorEvent final: public Event {
  public:
    ErrorEvent(jsg::Lock& js, const jsg::JsValue& error)
        : Event(kj::str("error")),
          error(js, error) {}

    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(ErrorEvent) {
      JSG_INHERIT(Event);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(error, getError);
    }

  private:
    jsg::JsRef<jsg::JsValue> error;

    jsg::JsValue getError(jsg::Lock& js) { return error.getHandle(js); }
  };

  class OpenEvent final: public Event {
  public:
    OpenEvent() : Event(kj::str("open")) {}
    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(OpenEvent) {
      JSG_INHERIT(Event);
    }
  };

  class MessageEvent final: public Event {
  public:
    explicit MessageEvent(kj::Maybe<kj::String>& type,
                 kj::String data,
                 kj::String lastEventId,
                 kj::Maybe<jsg::Url&> url)
        : Event(kj::mv(type).orDefault([] { return kj::str("message");})),
          data(kj::mv(data)),
          lastEventId(kj::mv(lastEventId)),
          origin(url.map([](auto& url) { return url.getOrigin(); })) {}

    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(MessageEvent) {
      JSG_INHERIT(Event);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(data, getData);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(lastEventId, getLastEventId);
    }

  private:
    kj::String data;
    kj::String lastEventId;
    kj::Maybe<kj::Array<const char>> origin;

    kj::StringPtr getData() { return data; }
    kj::StringPtr getLastEventId() { return lastEventId; }
    kj::Maybe<kj::ArrayPtr<const char>> getOrigin() {
      return origin.map([](auto& a) -> kj::ArrayPtr<const char> { return a.asPtr(); });
    }
  };

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

  static jsg::Ref<EventSource> constructor(jsg::Lock& js,
                                           kj::String url,
                                           jsg::Optional<EventSourceInit> init);

  kj::ArrayPtr<const char> getUrl() const {
    KJ_IF_SOME(i, impl) {
      return i.url.getHref();
    }
    return nullptr;
  }
  bool getWithCredentials() const { return false; }
  uint getReadyState() const { return static_cast<uint>(readyState); }

  void close(jsg::Lock& js);

  // A non-standard extension that creates an EventSource instance around a ReadableStream
  // instance. In this instance, automatic reconnection is disabled since there is no URL
  // or underlying fetch used. The ReadableStream instance must produce bytes. It will be
  // locked and disturbed, and will be read until it either ends or errors. Calling close()
  // will cause the stream to be canceled.
  static jsg::Ref<EventSource> from(jsg::Lock& js, jsg::Ref<ReadableStream> stream);

  kj::Maybe<jsg::JsValue> getOnOpen(jsg::Lock& js) {
    return onopenValue.map([&](jsg::JsRef<jsg::JsValue>& ref) -> jsg::JsValue {
      return ref.getHandle(js);
    });
  }
  void setOnOpen(jsg::Lock& js, jsg::JsValue value) {
    if (!value.isObject() && !value.isFunction()) {
      onopenValue = kj::none;
    } else {
      onopenValue = jsg::JsRef<jsg::JsValue>(js, value);
    }
  }
  kj::Maybe<jsg::JsValue> getOnMessage(jsg::Lock& js) {
    return onmessageValue.map([&](jsg::JsRef<jsg::JsValue>& ref) -> jsg::JsValue {
      return ref.getHandle(js);
    });
  }
  void setOnMessage(jsg::Lock& js, jsg::JsValue value) {
    if (!value.isObject() && !value.isFunction()) {
      onmessageValue = kj::none;
    } else {
      onmessageValue = jsg::JsRef<jsg::JsValue>(js, value);
    }
  }
  kj::Maybe<jsg::JsValue> getOnError(jsg::Lock& js) {
    return onerrorValue.map([&](jsg::JsRef<jsg::JsValue>& ref) -> jsg::JsValue {
      return ref.getHandle(js);
    });
  }
  void setOnError(jsg::Lock& js, jsg::JsValue value) {
    if (!value.isObject() && !value.isFunction()) {
      onerrorValue = kj::none;
    } else {
      onerrorValue = jsg::JsRef<jsg::JsValue>(js, value);
    }
  }

  JSG_RESOURCE_TYPE(EventSource) {
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
  void enqueueMessages(kj::Array<PendingMessage> messages);

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
    // Indicates that the server previous responded with no content after a
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
  kj::String lastEventId = kj::String();

  // Indicates that the close method has been previously called.
  bool closeCalled = false;

  // The EventSource spec defines onopen, onmessage, and onerror as prototype
  // properties on the class.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onopenValue;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onmessageValue;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onerrorValue;

  // The default reconnection wait time. This is fairly arbitrary and is left
  // entirely up to the implementation. The event stream can provide a new value
  static constexpr auto DEFAULT_RECONNECTION_TIME = 2 * kj::SECONDS;
  static constexpr uint32_t MIN_RECONNECTION_TIME = 1000;
  static constexpr uint32_t MAX_RECONNECTION_TIME = 10 * 1000;

  kj::Duration reconnectionTime = DEFAULT_RECONNECTION_TIME;

  void notifyOpen(jsg::Lock& js);
  void notifyError(jsg::Lock& js, const jsg::JsValue& error, bool reconnecting = false);
  void notifyMessages(jsg::Lock& js, kj::Array<PendingMessage> messages);

  void run(jsg::Lock& js,
           jsg::Ref<ReadableStream> stream,
           bool withReconnection = true,
           kj::Maybe<jsg::Ref<Response>> response = kj::none,
           kj::Maybe<jsg::Ref<Fetcher>> fetcher = kj::none);
  void start(jsg::Lock& js);
  void reconnect(jsg::Lock& js);
};

}  // namespace workerd::api

#define EW_EVENTSOURCE_ISOLATE_TYPES      \
  api::EventSource,                       \
  api::EventSource::ErrorEvent,           \
  api::EventSource::OpenEvent,            \
  api::EventSource::MessageEvent,         \
  api::EventSource::EventSourceInit
