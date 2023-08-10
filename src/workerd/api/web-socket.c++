// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "web-socket.h"
#include <workerd/jsg/jsg.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/util/sentry.h>
#include <kj/compat/url.h>

namespace workerd::api {

kj::StringPtr KJ_STRINGIFY(const WebSocket::NativeState& state) {
  // TODO(someday) We might care more about this `OneOf` than its which, that probably means
  // returning a kj::String instead.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(ac, WebSocket::AwaitingConnection) return "AwaitingConnection";
    KJ_CASE_ONEOF(aaoc, WebSocket::AwaitingAcceptanceOrCoupling)
        return "AwaitingAcceptanceOrCoupling";
    KJ_CASE_ONEOF(a, WebSocket::Accepted) return "Accepted";
    KJ_CASE_ONEOF(r, WebSocket::Released) return "Released";
  }
  KJ_UNREACHABLE;
}

IoOwn<WebSocket::Native> WebSocket::initNative(
    IoContext& ioContext,
    kj::WebSocket& ws,
    bool closedOutgoingConn) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<Accepted>(Accepted::Hibernatable{.ws = ws}, *nativeObj, ioContext);
  // We might have called `close()` when this WebSocket was previously active.
  // If so, we want to prevent any future calls to `send()`.
  nativeObj->closedOutgoing = closedOutgoingConn;
  return ioContext.addObject(kj::mv(nativeObj));
}

WebSocket::WebSocket(jsg::Lock& js,
    IoContext& ioContext,
    kj::WebSocket& ws,
    HibernationPackage package)
    : url(kj::mv(package.url)),
      protocol(kj::mv(package.protocol)),
      extensions(kj::mv(package.extensions)),
      serializedAttachment(kj::mv(package.serializedAttachment)),
      farNative(initNative(ioContext, ws, package.closedOutgoingConnection)),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())),
      locality(LOCAL) {}
  // This constructor is used when reinstantiating a websocket that had been hibernating, which is
  // why we can go straight to the Accepted state. However, note that we are actually in the
  // `Hibernatable` "sub-state"!

jsg::Ref<WebSocket> WebSocket::hibernatableFromNative(
    jsg::Lock& js,
    kj::WebSocket& ws,
    HibernationPackage package) {
  return jsg::alloc<WebSocket>(js, IoContext::current(), ws, kj::mv(package));
}

WebSocket::WebSocket(kj::Own<kj::WebSocket> native, Locality locality)
    : url(nullptr),
      farNative(nullptr),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())),
      locality(locality) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<AwaitingAcceptanceOrCoupling>(kj::mv(native));
  farNative = IoContext::current().addObject(kj::mv(nativeObj));
}

WebSocket::WebSocket(kj::String url, Locality locality)
    : url(kj::mv(url)),
      farNative(nullptr),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())),
      locality(locality) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<AwaitingConnection>();
  farNative = IoContext::current().addObject(kj::mv(nativeObj));
}

void WebSocket::initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket> prom) {

  auto& canceler = KJ_ASSERT_NONNULL(farNative->state.tryGet<AwaitingConnection>()).canceler;

  IoContext::current().awaitIo(js, canceler.wrap(kj::mv(prom)),
      [this, self = JSG_THIS](jsg::Lock& js, PackedWebSocket packedSocket) mutable {
    auto& native = *farNative;
    KJ_IF_MAYBE(pending, native.state.tryGet<AwaitingConnection>()) {
      // We've succeessfully established our web socket, we do not need to cancel anything.
      pending->canceler.release();
    }

    native.state.init<AwaitingAcceptanceOrCoupling>(AwaitingAcceptanceOrCoupling{
        IoContext::current().addObject(kj::mv(packedSocket.ws))});

    // both `protocol` and `extensions` start off as empty strings.
    // They become null if the connection is established and no protocol/extension was chosen.
    // https://html.spec.whatwg.org/multipage/web-sockets.html#dom-websocket-protocol
    KJ_IF_MAYBE(proto, packedSocket.proto) {
      protocol = kj::mv(*proto);
    } else {
      protocol = nullptr;
    }

    KJ_IF_MAYBE(ext, packedSocket.extensions) {
      extensions = kj::mv(*ext);
    } else {
      extensions = nullptr;
    }

    // Fire open event.
    internalAccept(js);
    dispatchOpen(js);
  }).catch_(js, [this, self = JSG_THIS](jsg::Lock& js, jsg::Value&& e) mutable {
    // Fire error event.
    // Sets readyState to CLOSING.
    farNative->closedIncoming = true;

    // Sets readyState to CLOSED.
    reportError(js, kj::mv(e));

    dispatchEventImpl(js,
        jsg::alloc<CloseEvent>(1006, kj::str("Failed to establish websocket connection"),
                                false));
  });
  // Note that in this attach we pass a strong reference to the WebSocket. The reference will be
  // dropped when either the connection promise completes or the IoContext is torn down,
  // whichever comes first.
}

namespace {

// See item 10 of https://datatracker.ietf.org/doc/html/rfc6455#section-4.1
bool validProtoToken(const kj::StringPtr protocol) {
  if (kj::size(protocol) == 0) {
    return false;
  }

  for (auto& c : protocol) {
    // Note that this also includes separators 0x20 (SP) and 0x09 (HT), so we don't need to check
    // for them below.
    if (c < 0x21 || 0x7E < c) {
      return false;
    }

    switch(c) {
      case '(':
      case ')':
      case '<':
      case '>':
      case '@':
      case ',':
      case ';':
      case ':':
      case '\\':
      case '/':
      case '[':
      case ']':
      case '?':
      case '=':
      case '{':
      case '}':
        return false;
      default:
        break;
    }
  }
  return true;
}

} // namespace

jsg::Ref<WebSocket> WebSocket::constructor(
    jsg::Lock& js,
    kj::String url,
    jsg::Optional<kj::OneOf<kj::Array<kj::String>, kj::String>> protocols) {

  auto& context = IoContext::current();

  // Check if we have a valid URL
  kj::Url urlRecord;
  kj::Maybe<kj::Exception> maybeException = kj::runCatchingExceptions([&]() {
    urlRecord = kj::Url::parse(url);
  });

  constexpr auto wsErr = "WebSocket Constructor: "_kj;

  JSG_REQUIRE(maybeException == nullptr, DOMSyntaxError, wsErr, "The url is invalid.");
  JSG_REQUIRE(urlRecord.scheme == "ws" || urlRecord.scheme == "wss", DOMSyntaxError, wsErr,
      "The url scheme must be ws or wss.");
  // We want the caller to pass `ws/wss` as per the spec, but FL would treat these as http in
  // `X-Forwarded-Proto`, so we want to ensure that `wss` results in `https`, not `http`.
  if (urlRecord.scheme == "ws") {
    urlRecord.scheme = kj::str("http");
  } else if (urlRecord.scheme == "wss") {
    urlRecord.scheme = kj::str("https");
  }

  JSG_REQUIRE(urlRecord.fragment == nullptr, DOMSyntaxError, wsErr,
      "The url fragment must be empty.");

  kj::HttpHeaders headers(context.getHeaderTable());
  auto client = context.getHttpClient(0, false, nullptr, "WebSocket::constructor"_kjc);

  // Set protocols header if necessary.
  KJ_IF_MAYBE(variant, protocols) {
    // String consisting of the protocol(s) we send to the server.
    kj::String protoString;

    KJ_SWITCH_ONEOF(*variant) {
      KJ_CASE_ONEOF(proto, kj::String) {
        JSG_REQUIRE(validProtoToken(proto), DOMSyntaxError, wsErr,
            "The protocol header token is invalid.");
        protoString = kj::mv(proto);
      }
      KJ_CASE_ONEOF(protoArr, kj::Array<kj::String>) {
        JSG_REQUIRE(kj::size(protoArr) > 0, DOMSyntaxError, wsErr,
            "The protocols array cannot be empty.");
        // Search for duplicates by checking for their presence in the set.
        kj::HashSet<kj::String> present;

        for (const auto& proto: protoArr) {
          JSG_REQUIRE(validProtoToken(proto), DOMSyntaxError, wsErr,
              "One of the protocol header tokens is invalid.");
          JSG_REQUIRE(!present.contains(proto), DOMSyntaxError, wsErr,
              "The protocols header cannot have repeating values.");

          present.insert(kj::str(proto));
        }
        const auto delim = ", "_kj;
        protoString = kj::str(kj::delimited(protoArr, delim));
      }
    }
    auto protoHeaderId = context.getHeaderIds().secWebSocketProtocol;
    headers.set(protoHeaderId, kj::mv(protoString));
  }

  auto connUrl = urlRecord.toString();
  auto ws = jsg::alloc<WebSocket>(kj::mv(url), Locality::REMOTE);

  headers.set(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS, kj::str("permessage-deflate"));
  // By default, browsers set the compression extension header for `new WebSocket()`.

  if (!FeatureFlags::get(js).getWebSocketCompression()) {
    // If we haven't enabled the websocket compression compatibility flag, strip the header from the
    // subrequest.
    headers.unset(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS);
  }

  auto prom = ([](auto& context, auto connUrl, auto headers, auto client)
      -> kj::Promise<PackedWebSocket> {
    auto response = co_await client->openWebSocket(connUrl, headers);

    JSG_REQUIRE(response.statusCode == 101, TypeError,
        "Failed to establish the WebSocket connection: expected server to reply with HTTP "
        "status code 101 (switching protocols), but received ", response.statusCode, " instead.");

    KJ_SWITCH_ONEOF(response.webSocketOrBody) {
      KJ_CASE_ONEOF(webSocket, kj::Own<kj::WebSocket>) {
        auto maybeProtoPtr = response.headers->get(context.getHeaderIds().secWebSocketProtocol);
        auto maybeExtensionsPtr = response.headers->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS);

        kj::Maybe<kj::String> maybeProto;
        kj::Maybe<kj::String> maybeExtensions;

        KJ_IF_MAYBE(proto, maybeProtoPtr) {
          maybeProto = kj::str(*proto);
        }

        KJ_IF_MAYBE(extensions, maybeExtensionsPtr) {
          maybeExtensions = kj::str(*extensions);
        }

        co_return PackedWebSocket{
          .ws = webSocket.attach(kj::mv(client)),
          .proto = kj::mv(maybeProto),
          .extensions = kj::mv(maybeExtensions)
        };
      }
      KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
        JSG_FAIL_REQUIRE(TypeError,
            "Worker received body in a response to a request for a WebSocket.");
      }
    }
    KJ_UNREACHABLE
  })(context, kj::mv(connUrl), kj::mv(headers), kj::mv(client));

  ws->initConnection(js, kj::mv(prom));

  return ws;
}

kj::Promise<DeferredProxy<void>> WebSocket::couple(kj::Own<kj::WebSocket> other) {
  auto& native = *farNative;
  JSG_REQUIRE(!native.state.is<AwaitingConnection>(), TypeError,
      "Can't return WebSocket in a Response if it was created with `new WebSocket()`");
  JSG_REQUIRE(!native.state.is<Released>(), TypeError,
      "Can't return WebSocket that was already used in a response.");
  KJ_IF_MAYBE(state, native.state.tryGet<Accepted>()) {
    if (state->isHibernatable()) {
      JSG_FAIL_REQUIRE(TypeError,
          "Can't return WebSocket in a Response after calling acceptWebSocket().");
    } else {
      JSG_FAIL_REQUIRE(TypeError, "Can't return WebSocket in a Response after calling accept().");
    }
  }

  // Tear down the IoOwn since we now need to extend the WebSocket to a `DeferredProxy` promise.
  // This works because the `DeferredProxy` ends on the same event loop, but after the request
  // context goes away.
  kj::Own<kj::WebSocket> self = kj::mv(KJ_ASSERT_NONNULL(
      native.state.tryGet<AwaitingAcceptanceOrCoupling>()).ws);
  native.state.init<Released>();

  auto& context = IoContext::current();

  auto upstream = other->pumpTo(*self);
  auto downstream = self->pumpTo(*other);

  if (locality == LOCAL) {
    // We're terminating the WebSocket in this worker, so the upstream promise (which pumps
    // messages from the client to this worker) counts as something the request is waiting for.
    upstream = upstream.attach(context.registerPendingEvent());
  }

  // We need to use `eagerlyEvaluate()` on both inputs to `joinPromises` to work around the awkward
  // behavior of `joinPromises` lazily-evaluating tail continuations.
  auto promise = kj::joinPromises(kj::arr(upstream.eagerlyEvaluate(nullptr),
                                          downstream.eagerlyEvaluate(nullptr)))
      .attach(kj::mv(self), kj::mv(other));

  if (locality == LOCAL) {
    // Since the WebSocket is terminated locally, we need the IoContext to stay live while
    // it is active.
    co_await promise;
    co_return DeferredProxy<void> { kj::READY_NOW };
  } else {
    // Since the WebSocket is just proxying through, we can do the pump in a deferred proxy task.
    // Note that we don't need to (and can't) register any pending events in this case since the
    // IoContext is free to go away at this point.
    co_return DeferredProxy<void> { kj::mv(promise) };
  }
}

void WebSocket::accept(jsg::Lock& js) {
  auto& native = *farNative;
  JSG_REQUIRE(!native.state.is<AwaitingConnection>(), TypeError,
      "Websockets obtained from the 'new WebSocket()' constructor cannot call accept");
  JSG_REQUIRE(!native.state.is<Released>(), TypeError,
      "Can't accept() WebSocket that was already used in a response.");

  KJ_IF_MAYBE(accepted, native.state.tryGet<Accepted>()) {
    JSG_REQUIRE(!accepted->isHibernatable(), TypeError,
        "Can't accept() WebSocket after enabling hibernation.");
    // Technically, this means it's okay to invoke `accept()` once a `new WebSocket()` resolves to
    // an established connection. This is probably okay? It might spare the worker devs a class of
    // errors they do not care care about.
    return;
  }

  internalAccept(js);
}

void WebSocket::internalAccept(jsg::Lock& js) {
  auto& native = *farNative;
  auto nativeWs = kj::mv(KJ_ASSERT_NONNULL(native.state.tryGet<AwaitingAcceptanceOrCoupling>()).ws);
  native.state.init<Accepted>(kj::mv(nativeWs), native, IoContext::current());
  return startReadLoop(js);
}

WebSocket::Accepted::Accepted(kj::Own<kj::WebSocket> wsParam, Native& native, IoContext& context)
    : ws(kj::mv(wsParam)),
      whenAbortedTask(createAbortTask(native, context)) {
  KJ_IF_MAYBE(a, context.getActor()) {
    auto& metrics = a->getMetrics();
    metrics.webSocketAccepted();

    // Save the metrics object for the destructor since the IoContext may not be accessible
    // there.
    actorMetrics = kj::addRef(metrics);
  }
}

WebSocket::Accepted::Accepted(Hibernatable wsParam, Native& native, IoContext& context)
    : ws(kj::mv(wsParam)),
      whenAbortedTask(createAbortTask(native, context)) {
  KJ_IF_MAYBE(a, context.getActor()) {
    auto& metrics = a->getMetrics();
    metrics.webSocketAccepted();

    // Save the metrics object for the destructor since the IoContext may not be accessible
    // there.
    actorMetrics = kj::addRef(metrics);
  }
}

kj::Promise<void> WebSocket::Accepted::createAbortTask(Native& native, IoContext& context) {
  try {
    // whenAborted() is theoretically not supposed to throw, but some code paths, like
    // AbortableWebSocket and Cap'n Proto disconnects, may end up throwing DISCONNECTED. Treat
    // exceptions the same as if `whenAborted()` finished normally -- but log in catch catch
    // block if it's not DISCONNECTED.
    co_await ws->whenAborted();

    // Other end disconnected prematurely. We may be able to clean up our state.
    native.outgoingAborted = true;
    if (!native.isPumping && native.closedIncoming) {
      // We can safely destroy the underlying WebSocket as it is no longer in use.
      // HACK: Replacing the state will delete `whenAbortedTask`, which is the task that is
      //   currently executing, which will crash. We know we're at the end of the task here
      //   so detach it as a work-around.
      whenAbortedTask.detach([](auto&&) {});
      native.state.init<Released>();
    } else {
      // Either we haven't received the incoming disconnect yet, or there are writes
      // in-flight. In either case, we need to wait for those to happen before we destroy the
      // underlying object, or we might have a UAF situation. Those other operations should
      // fail shortly and notice the `outgoingAborted` flag when they do.
    }
  } catch (...) {
    auto ex = kj::getCaughtExceptionAsKj();
    if (ex.getType() != kj::Exception::Type::DISCONNECTED) {
      LOG_EXCEPTION("webSocketWhenAborted", ex);
    }
  }
}

WebSocket::Accepted::~Accepted() noexcept(false) {
  KJ_IF_MAYBE(a, actorMetrics) {
    a->get()->webSocketClosed();
  }
}

void WebSocket::startReadLoop(jsg::Lock& js) {
  // If the kj::WebSocket happens to be an AbortableWebSocket (see util/abortable.h), then
  // calling readLoop here could throw synchronously if the canceler has already been tripped.
  // Using kj::evalNow() here let's us capture that and handle correctly.
  //
  // We catch exceptions and return Maybe<Exception> instead since we want to handle the exceptions
  // in awaitIo() below, but we don't want the KJ exception converted to JavaScript before we can
  // examine it.
  kj::Promise<kj::Maybe<kj::Exception>> promise = readLoop();

  auto& context = IoContext::current();

  if (locality == REMOTE) {
    promise = promise.attach(context.registerPendingEvent());
  }

  // We put the read loop in a `waitUntil`, since there would otherwise be a race condition between
  // delivering the final close message and the request being canceled due to client disconnect.
  // This `waitUntil` will not significantly extend the lifetime of the request in practice, as the
  // request otherwise ends when the client disconnects, and the read loop will also end when the
  // client disconnects -- we just want to ensure that they happen in the right order.
  //
  // TODO(bug): Using waitUntil() for this purpose is only correct for WebSockets originating from
  //   the eyeball. For an outgoing WebSocket, we should just do addTask(). Alternatively, perhaps
  //   we need to adjust the cancellation logic to wait for whenThreadIdle() before cancelling,
  //   which would then allow close messages to be delivered from eyeball connections without any
  //   use of waitUntil().
  //
  // TODO(cleanup): We have to use awaitIoLegacy() so that we can handle registerPendingEvent()
  //   manually. Ideally, we'd refactor things such that a WebSocketPair where both ends are
  //   accepted locally is implemented completely in JavaScript space, using jsg::Promise instead
  //   of kj::Promise, and then only use awaitIo() on truely remote WebSockets.
  // TODO(cleanup): Should addWaitUntil() take jsg::Promise instead of kj::Promise?
  context.addWaitUntil(context.awaitJs(js, context.awaitIoLegacy(kj::mv(promise))
      .then(js, [this, thisHandle = JSG_THIS]
                (jsg::Lock& js, kj::Maybe<kj::Exception>&& maybeError) mutable {
    auto& native = *farNative;
    KJ_IF_MAYBE(e, maybeError) {
      if (!native.closedIncoming && e->getType() == kj::Exception::Type::DISCONNECTED) {
        // Report premature disconnect or cancel as a close event.
        dispatchEventImpl(js, jsg::alloc<CloseEvent>(
            1006, kj::str("WebSocket disconnected without sending Close frame."), false));
        native.closedIncoming = true;
        // If there are no further messages to send, so we can discard the underlying connection.
        tryReleaseNative(js);
      } else {
        native.closedIncoming = true;
        reportError(js, kj::cp(*e));
        kj::throwFatalException(kj::mv(*e));
      }
    }
  })));
}

void WebSocket::send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message) {
  auto& native = *farNative;
  JSG_REQUIRE(!native.closedOutgoing, TypeError, "Can't call WebSocket send() after close().");
  if (native.outgoingAborted || native.state.is<Released>()) {
    // Per the spec, we should silently ignore send()s that happen after the connection is closed.
    // NOTE: The spec claims send() should also silently ignore messages sent after a close message
    //   has been sent or received cleanly. We ignore this advice:
    // * If close has been sent, i.e. close() has been called, then calling send() is clearly a
    //   bug, and we'd like to help people debug, so we throw an exception above. (This point is
    //   debatable, we could change it.)
    // * It makes no sense that *receiving* a close message should prevent further calls to send().
    //   The spec seems broken here. What if you need to send a couple final messages for a clean
    //   shutdown?
    return;
  } else if (awaitingHibernatableError()) {
    // Ready for the hibernatable error event state, after encountering an error, the websocket
    // isn't able to send outbound messages; let's release it.
    tryReleaseNative(js);
    return;
  }

  JSG_REQUIRE(native.state.is<Accepted>(), TypeError,
      "You must call one of accept() or state.acceptWebSocket() on this WebSocket before sending "\
      "messages.");

  auto maybeOutputLock = IoContext::current().waitForOutputLocksIfNecessary();
  auto msg = [&]() -> kj::WebSocket::Message {
    KJ_SWITCH_ONEOF(message) {
      KJ_CASE_ONEOF(text, kj::String) {
        return kj::mv(text);
        break;
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        return kj::mv(data);
        break;
      }
    }

    KJ_UNREACHABLE;
  }();
  outgoingMessages->insert(GatedMessage{kj::mv(maybeOutputLock), kj::mv(msg)});

  ensurePumping(js);
}

void WebSocket::close(
    jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<kj::String> reason) {
  auto& native = *farNative;

  // Handle close before connection is established for websockets obtained through `new WebSocket()`.
  KJ_IF_MAYBE(pending, native.state.tryGet<AwaitingConnection>()) {
    pending->canceler.cancel(kj::str("Called close before connection was established."));

    // Strictly speaking, we might not be all the way released by now, but we definitely shouldn't
    // worry about canceling again.
    native.state.init<Released>();
    return;
  }

  if (native.closedOutgoing || native.outgoingAborted || native.state.is<Released>()) {
    // See comments in send(), above, which also apply here. Note that we opt to ignore a
    // double-close() per spec, whereas send()-after-close() throws (off-spec).

    return;
  } else if (awaitingHibernatableError()) {
    // Ready for the hibernatable error event state, after encountering an error, the websocket
    // isn't able to send outbound messages; let's release it.
    tryReleaseNative(js);
    return;
  }
  JSG_REQUIRE(native.state.is<Accepted>(), TypeError,
      "You must call one of accept() or state.acceptWebSocket() on this WebSocket before sending "\
      "messages.");

  assertNoError(js);

  KJ_IF_MAYBE(c, code) {
    JSG_REQUIRE(*c >= 1000 && *c < 5000 && *c != 1004 && *c != 1005 && *c != 1006 && *c != 1015,
                 TypeError, "Invalid WebSocket close code: ", *c, ".");
  }
  if (reason != nullptr) {
    // The default code of 1005 cannot have a reason, per the standard, so if a reason is specified
    // then there must be a code, too.
    JSG_REQUIRE(code != nullptr, TypeError,
        "If you specify a WebSocket close reason, you must also specify a code.");
  }

  outgoingMessages->insert(GatedMessage{
      IoContext::current().waitForOutputLocksIfNecessary(),
      kj::WebSocket::Close {
        // Code 1005 actually translates to sending a close message with no body on the wire.
        static_cast<uint16_t>(code.orDefault(1005)),
        kj::mv(reason).orDefault(nullptr),
      },
  });

  native.closedOutgoing = true;
  closedOutgoingForHib = true;
  ensurePumping(js);
}

int WebSocket::getReadyState() {
  auto& native = *farNative;
  if ((native.closedIncoming && native.closedOutgoing) || error != nullptr) {
    return READY_STATE_CLOSED;
  } else if (native.closedIncoming || native.closedOutgoing) {
    // Bizarrely, the spec uses the same state for a close message having been sent *or* received,
    // even though these are very different states from the point of view of the application.
    return READY_STATE_CLOSING;
  } else if (native.state.is<AwaitingConnection>()) {
    return READY_STATE_CONNECTING;
  }
  return READY_STATE_OPEN;
}

bool WebSocket::isAccepted() {
  return farNative->state.is<Accepted>();
}

bool WebSocket::isReleased() {
  return farNative->state.is<Released>();
}

kj::Maybe<kj::String> WebSocket::getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx) {
  KJ_SWITCH_ONEOF(farNative->state) {
    KJ_CASE_ONEOF(ws, AwaitingConnection) {
      return nullptr;
    }
    KJ_CASE_ONEOF(container, AwaitingAcceptanceOrCoupling) {
      return container.ws->getPreferredExtensions(ctx);
    }
    KJ_CASE_ONEOF(container, Accepted) {
      return container.ws->getPreferredExtensions(ctx);
    }
    KJ_CASE_ONEOF(container, Released) {
      return nullptr;
    }
  }
  return nullptr;
}

kj::Maybe<kj::StringPtr> WebSocket::getUrl() {
  return url.map([](kj::StringPtr value){ return value; });
}

kj::Maybe<kj::StringPtr> WebSocket::getProtocol() {
  return protocol.map([](kj::StringPtr value){ return value; });
}

kj::Maybe<kj::StringPtr> WebSocket::getExtensions() {
  return extensions.map([](kj::StringPtr value){ return value; });
}

kj::Maybe<v8::Local<v8::Value>> WebSocket::deserializeAttachment(jsg::Lock& js) {
  return serializedAttachment.map([&](kj::ArrayPtr<byte> attachment) {
    jsg::Deserializer deserializer(js, attachment, nullptr, nullptr,
        jsg::Deserializer::Options {
      .version = 15,
      .readHeader = true,
    });

    return deserializer.readValue();
  });
}

void WebSocket::serializeAttachment(jsg::Lock& js, v8::Local<v8::Value> attachment) {
  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(attachment);
  auto released = serializer.release();
  JSG_REQUIRE(released.data.size() <= MAX_ATTACHMENT_SIZE, Error,
      "A WebSocket 'attachment' cannot be larger than ",  MAX_ATTACHMENT_SIZE, " bytes." \
      "'attachment' was ", released.data.size(), " bytes.");
  serializedAttachment = kj::mv(released.data);
}

void WebSocket::setAutoResponseTimestamp(kj::Maybe<kj::Date> time) {
  autoResponseTimestamp = time;
}


kj::Maybe<kj::Date> WebSocket::getAutoResponseTimestamp() {
  return autoResponseTimestamp;
}

void WebSocket::dispatchOpen(jsg::Lock& js) {
  dispatchEventImpl(js, jsg::alloc<Event>("open"));
}

void WebSocket::ensurePumping(jsg::Lock& js) {
  auto& native = *farNative;
  if (!native.isPumping) {
    auto& context = IoContext::current();
    auto& accepted = KJ_ASSERT_NONNULL(native.state.tryGet<Accepted>());
    auto promise = kj::evalNow([&]() {
      return accepted.canceler.wrap(pump(context, *outgoingMessages, *accepted.ws, native));
    });

    // TODO(cleanup): We use awaitIoLegacy() here because we don't want this to count as a pending
    //   event if this is a WebSocketPair with the other end being handled in the same isolate.
    //   In that case, the pump can hang if accept() is never called on the other end. Ideally,
    //   this scenario would be handled in-isolate using jsg::Promise, but that would take some
    //   refactoring.
    context.awaitIoLegacy(kj::mv(promise)).then(js, [this, thisHandle = JSG_THIS](jsg::Lock& js) {
      auto& native = *farNative;
      if (native.outgoingAborted) {
        if (awaitingHibernatableRelease()) {
          // We have a hibernatable websocket -- we don't want to dispatch a regular error event.
          tryReleaseNative(js);
        } else {
          // Apparently, the peer stopped accepting messages (probably, disconnected entirely), but
          // this didn't cause our writes to fail, maybe due to timing. Let's set the error now.
          reportError(js, KJ_EXCEPTION(DISCONNECTED, "WebSocket peer disconnected"));
        }
      } else if (native.closedIncoming && native.closedOutgoing) {
        if (native.state.is<Accepted>()) {
          // Native WebSocket no longer needed; release.
          native.state.init<Released>();
        } else if (native.state.is<Released>()) {
          // While we were awaiting the jsg::Promise, someone else released our state. That's fine.
        } else {
          KJ_FAIL_ASSERT("Unexpected native web socket state", native.state);
        }
      }
    }, [this, thisHandle = JSG_THIS](jsg::Lock& js, jsg::Value&& exception) mutable {
      if (awaitingHibernatableRelease()) {
        // We have a hibernatable websocket -- we don't want to dispatch a regular error event.
        tryReleaseNative(js);
      } else {
        reportError(js, kj::mv(exception));
      }
    });
  }
}

namespace {

size_t countBytesFromMessage(const kj::WebSocket::Message& message) {
  // This does not count the extra data of the RPC frame or the savings from any compression.
  // We're incentivizing customers to use reasonably sized messages, not trying to get an exact
  // count of how many bytes went over the wire.

  KJ_SWITCH_ONEOF(message) {
    KJ_CASE_ONEOF(s, kj::String) {
      return s.size();
    }
    KJ_CASE_ONEOF(a, kj::Array<byte>) {
      return a.size();
    }
    KJ_CASE_ONEOF(c, kj::WebSocket::Close) {
      // If we include the size of the close code, that could incentivize our customers to omit
      // sending Close frames when appropriate. The same cannot be said for the close reason since
      // someone could encapsulate their final message in it to save costs.
      return c.reason.size();
    }
  }

  KJ_UNREACHABLE;
}

} // namespace

kj::Promise<void> WebSocket::pump(
    IoContext& context, OutgoingMessagesMap& outgoingMessages, kj::WebSocket& ws, Native& native) {
  KJ_ASSERT(!native.isPumping);
  native.isPumping = true;
  KJ_DEFER({
    // We use a KJ_DEFER to set native.isPumping = false to ensure that it happens -- we had a bug
    // in the past where this was handled by the caller of WebSocket::pump() and it allowed for
    // messages to get stuck in `outgoingMessages` until the pump task was restarted.
    native.isPumping = false;

    // Either we were already through all our outgoing messages or we experienced failure/
    // cancellation and cannot send these anyway.
    outgoingMessages.clear();
  });

  while (outgoingMessages.size() > 0) {
    GatedMessage gatedMessage = outgoingMessages.release(*outgoingMessages.ordered().begin());
    KJ_IF_MAYBE(promise, gatedMessage.outputLock) {
      co_await *promise;
    }

    auto size = countBytesFromMessage(gatedMessage.message);

    KJ_SWITCH_ONEOF(gatedMessage.message) {
      KJ_CASE_ONEOF(text, kj::String) {
        co_await ws.send(text);
        break;
      }
      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        co_await ws.send(data);
        break;
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        co_await ws.close(close.code, close.reason);
        break;
      }
    }

    KJ_IF_MAYBE(a, context.getActor()) {
      a->getMetrics().sentWebSocketMessage(size);
    }
  }
}

void WebSocket::tryReleaseNative(jsg::Lock& js) {
  // If the native WebSocket is no longer needed (the connection closed) and there are no more
  // messages to send, we can discard the underlying connection.
  auto& native = *farNative;
  if ((native.closedOutgoing || native.outgoingAborted) && !native.isPumping) {
    // Native WebSocket no longer needed; release.
    KJ_ASSERT(native.state.is<Accepted>());
    native.state.init<Released>();
  }
}

kj::Promise<kj::Maybe<kj::Exception>> WebSocket::readLoop() {
  try {
    // Note that we'll throw if the websocket has enabled hibernation.
    auto& ws = *KJ_REQUIRE_NONNULL(
        KJ_ASSERT_NONNULL(farNative->state.tryGet<Accepted>()).ws.getIfNotHibernatable());
    auto& context = IoContext::current();
    while (true) {
      auto message = co_await ws.receive();

      context.getLimitEnforcer().topUpActor();
      KJ_IF_MAYBE(a, context.getActor()) {
        auto size = countBytesFromMessage(message);
        a->getMetrics().receivedWebSocketMessage(size);
      }

      // Re-enter the context with context.run(). This is arguably a bit unusual compared to other
      // I/O which is delivered by return from context.awaitIo(), but the difference here is that we
      // have a long stream of events over time. It makes sense to use context.run() each time a new
      // event arrives.
      // TODO(cleanup): The way context.run is defined, a capturing lambda is required here, which
      // is a bit unfortunate. We could simply things somewhat with a variation that would allow
      // something like context.run(handleMessage, *this, kj::mv(message)) where the acquired lock,
      // and the additional arguments are passed into handleMessage, avoiding the need for the
      // lambda here entirely.
      auto result = co_await context.run([this, message=kj::mv(message)](auto& wLock) mutable {
        auto& native = *farNative;
        jsg::Lock& js = wLock;
        KJ_SWITCH_ONEOF(message) {
          KJ_CASE_ONEOF(text, kj::String) {
            dispatchEventImpl(js, jsg::alloc<MessageEvent>(js, js.wrapString(text)));
          }
          KJ_CASE_ONEOF(data, kj::Array<byte>) {
            dispatchEventImpl(js, jsg::alloc<MessageEvent>(js, js.wrapBytes(kj::mv(data))));
          }
          KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
            native.closedIncoming = true;
            dispatchEventImpl(js, jsg::alloc<CloseEvent>(close.code, kj::mv(close.reason), true));
            // Native WebSocket no longer needed; release.
            tryReleaseNative(js);
            return false;
          }
        }

        return true;
      });

      if (!result) co_return nullptr;
    }
    KJ_UNREACHABLE;
  } catch (...) {
    co_return kj::getCaughtExceptionAsKj();
  }
}

jsg::Ref<WebSocketPair> WebSocketPair::constructor() {
  auto pipe = kj::newWebSocketPipe();
  auto pair = jsg::alloc<WebSocketPair>(
      jsg::alloc<WebSocket>(kj::mv(pipe.ends[0]), WebSocket::LOCAL),
      jsg::alloc<WebSocket>(kj::mv(pipe.ends[1]), WebSocket::LOCAL));
  auto first = pair->getFirst();
  auto second = pair->getSecond();

  first->setMaybePair(second.addRef());
  second->setMaybePair(first.addRef());
  return kj::mv(pair);
}

void ErrorEvent::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(error);
}

void WebSocket::reportError(jsg::Lock& js, kj::Exception&& e) {
  jsg::Value err = js.exceptionToJs(kj::cp(e));
  reportError(js, kj::mv(err));
}

void WebSocket::reportError(jsg::Lock& js, jsg::Value err) {
  // If this is the first error, raise the error event.
  if (error == nullptr) {
    auto msg = kj::str(v8::Exception::CreateMessage(js.v8Isolate, err.getHandle(js))->Get());
    error = err.addRef(js);

    dispatchEventImpl(js, jsg::alloc<ErrorEvent>(js, kj::mv(msg), kj::mv(err)));

    // After an error we don't allow further send()s. If the receive loop has also ended then we
    // can destroy the connection. Note that we don't set closedOutgoing = true because that flag
    // is specifically to indicate that `close()` has been called, and it causes `send()` to throw
    // an exception complaining specifically that `close()` was called, which would be
    // inappropriate in this case.
    auto& native = *farNative;
    native.outgoingAborted = true;
    if (native.closedIncoming && !native.isPumping) {
      KJ_IF_MAYBE(pending, native.state.tryGet<AwaitingConnection>()) {
        // Nothing worth canceling if we're reporting an error from the connection establishment
        // continuations.
        pending->canceler.release();
      }

      // We're no longer pumping so let's make sure we release the native connection here.
      native.state.init<Released>();
    }
  }
}

void WebSocket::assertNoError(jsg::Lock& js) {
  KJ_IF_MAYBE(e, error) {
    js.throwException(e->addRef(js));
  }
}

}  // namespace workerd::api
