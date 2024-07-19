// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "web-socket.h"
#include "events.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
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
    kj::Array<kj::StringPtr> tags,
    bool closedOutgoingConn) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<Accepted>(Accepted::Hibernatable {
      .ws = ws,
      .tagsRef = kj::mv(tags) },
      *nativeObj, ioContext);
  // We might have called `close()` when this WebSocket was previously active.
  // If so, we want to prevent any future calls to `send()`.
  nativeObj->closedOutgoing = closedOutgoingConn;
  autoResponseStatus.isClosed = nativeObj->closedOutgoing;
  return ioContext.addObject(kj::mv(nativeObj));
}

WebSocket::WebSocket(jsg::Lock& js,
    IoContext& ioContext,
    kj::WebSocket& ws,
    HibernationPackage package)
    : weakRef(kj::refcounted<WeakRef<WebSocket>>(kj::Badge<WebSocket> {}, *this)),
      url(kj::mv(package.url)),
      protocol(kj::mv(package.protocol)),
      extensions(kj::mv(package.extensions)),
      serializedAttachment(kj::mv(package.serializedAttachment)),
      farNative(
          initNative(ioContext,
              ws,
              kj::mv(KJ_REQUIRE_NONNULL(package.maybeTags)),
              package.closedOutgoingConnection)),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())) {}
  // This constructor is used when reinstantiating a websocket that had been hibernating, which is
  // why we can go straight to the Accepted state. However, note that we are actually in the
  // `Hibernatable` "sub-state"!

jsg::Ref<WebSocket> WebSocket::hibernatableFromNative(
    jsg::Lock& js,
    kj::WebSocket& ws,
    HibernationPackage package) {
  return jsg::alloc<WebSocket>(js, IoContext::current(), ws, kj::mv(package));
}

WebSocket::WebSocket(kj::Own<kj::WebSocket> native)
    : weakRef(kj::refcounted<WeakRef<WebSocket>>(kj::Badge<WebSocket> {}, *this)),
      url(kj::none),
      farNative(nullptr),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<AwaitingAcceptanceOrCoupling>(kj::mv(native));
  farNative = IoContext::current().addObject(kj::mv(nativeObj));
}

WebSocket::WebSocket(kj::String url)
    : weakRef(kj::refcounted<WeakRef<WebSocket>>(kj::Badge<WebSocket> {}, *this)),
      url(kj::mv(url)),
      farNative(nullptr),
      outgoingMessages(IoContext::current().addObject(kj::heap<OutgoingMessagesMap>())) {
  auto nativeObj = kj::heap<Native>();
  nativeObj->state.init<AwaitingConnection>();
  farNative = IoContext::current().addObject(kj::mv(nativeObj));
}

void WebSocket::initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket> prom) {

  auto& canceler = KJ_ASSERT_NONNULL(farNative->state.tryGet<AwaitingConnection>()).canceler;

  IoContext::current().awaitIo(js, canceler.wrap(kj::mv(prom)),
      [this, self = JSG_THIS](jsg::Lock& js, PackedWebSocket packedSocket) mutable {
    auto& native = *farNative;
    KJ_IF_SOME(pending, native.state.tryGet<AwaitingConnection>()) {
      // We've succeessfully established our web socket, we do not need to cancel anything.
      pending.canceler.release();
    }

    native.state.init<AwaitingAcceptanceOrCoupling>(AwaitingAcceptanceOrCoupling{
        IoContext::current().addObject(kj::mv(packedSocket.ws))});

    // both `protocol` and `extensions` start off as empty strings.
    // They become null if the connection is established and no protocol/extension was chosen.
    // https://html.spec.whatwg.org/multipage/web-sockets.html#dom-websocket-protocol
    KJ_IF_SOME(proto, packedSocket.proto) {
      protocol = kj::mv(proto);
    } else {
      protocol = kj::none;
    }

    KJ_IF_SOME(ext, packedSocket.extensions) {
      extensions = kj::mv(ext);
    } else {
      extensions = kj::none;
    }

    // Fire open event.
    internalAccept(js, IoContext::current().getCriticalSection());
    dispatchOpen(js);
  }).catch_(js, [this, self = JSG_THIS](jsg::Lock& js, jsg::Value&& e) mutable {
    // Fire error event.
    // Sets readyState to CLOSING.
    farNative->closedIncoming = true;

    // Sets readyState to CLOSED.
    reportError(js, jsg::JsValue(e.getHandle(js)).addRef(js));

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

  JSG_REQUIRE(maybeException == kj::none, DOMSyntaxError, wsErr, "The url is invalid.");
  JSG_REQUIRE(urlRecord.scheme == "ws" || urlRecord.scheme == "wss", DOMSyntaxError, wsErr,
      "The url scheme must be ws or wss.");
  // We want the caller to pass `ws/wss` as per the spec, but FL would treat these as http in
  // `X-Forwarded-Proto`, so we want to ensure that `wss` results in `https`, not `http`.
  if (urlRecord.scheme == "ws") {
    urlRecord.scheme = kj::str("http");
  } else if (urlRecord.scheme == "wss") {
    urlRecord.scheme = kj::str("https");
  }

  JSG_REQUIRE(urlRecord.fragment == kj::none, DOMSyntaxError, wsErr,
      "The url fragment must be empty.");

  kj::HttpHeaders headers(context.getHeaderTable());
  auto client = context.getHttpClient(0, false, kj::none, "WebSocket::constructor"_kjc);

  // Set protocols header if necessary.
  KJ_IF_SOME(variant, protocols) {
    // String consisting of the protocol(s) we send to the server.
    kj::String protoString;

    KJ_SWITCH_ONEOF(variant) {
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
  auto ws = jsg::alloc<WebSocket>(kj::mv(url));

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

        KJ_IF_SOME(proto, maybeProtoPtr) {
          maybeProto = kj::str(proto);
        }

        KJ_IF_SOME(extensions, maybeExtensionsPtr) {
          maybeExtensions = kj::str(extensions);
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

kj::Promise<DeferredProxy<void>> WebSocket::couple(kj::Own<kj::WebSocket> other, RequestObserver& request) {
  auto& native = *farNative;
  JSG_REQUIRE(!native.state.is<AwaitingConnection>(), TypeError,
      "Can't return WebSocket in a Response if it was created with `new WebSocket()`");
  JSG_REQUIRE(!native.state.is<Released>(), TypeError,
      "Can't return WebSocket that was already used in a response.");
  KJ_IF_SOME(state, native.state.tryGet<Accepted>()) {
    if (state.isHibernatable()) {
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

  auto tryGetPeer = [&]() -> kj::Maybe<WebSocket&> {
    KJ_IF_SOME(p, peer) {
      return p->tryGet();
    }
    return kj::none;
  };
  auto isHibernatable = [&](workerd::api::WebSocket& ws) {
    KJ_IF_SOME(state, ws.farNative->state.tryGet<Accepted>()) {
      return state.isHibernatable();
    }
    return false;
  };
  KJ_IF_SOME(p, tryGetPeer()) {
    // We're terminating the WebSocket in this worker, so the upstream promise (which pumps
    // messages from the client to this worker) counts as something the request is waiting for.
    upstream = upstream.attach(context.registerPendingEvent());

    // We can observe websocket traffic in both directions by attaching an observer to the peer
    // websocket which terminates in the worker.
    KJ_IF_SOME(observer, request.tryCreateWebSocketObserver()) {
      p.observer = kj::mv(observer);
    }
  }

  // We need to use `eagerlyEvaluate()` on both inputs to `joinPromises` to work around the awkward
  // behavior of `joinPromises` lazily-evaluating tail continuations.
  auto promise = kj::joinPromises(kj::arr(upstream.eagerlyEvaluate(nullptr),
                                          downstream.eagerlyEvaluate(nullptr)))
      .attach(kj::mv(self), kj::mv(other));

  KJ_IF_SOME(peer, tryGetPeer()) {
    // Since the WebSocket is terminated locally, we generally want the request and associated
    // IoContext to stay alive until the WebSocket connection has terminated.
    //
    // However, there is one exception to this: when the WebSocket is hibernatable, we don't want
    // the existence of this connection to prevent the actor from being evicted, so we fall through
    // to deferred proxying in this case.
    if (!isHibernatable(peer)) {
      co_await promise;
      co_return;
    }
  }

  // Either:
  // 1. This websocket is just proxying through, in which case we can allow the IoContext to go
  // away while still being able to successfully pump the websocket connection.
  // 2. This is a hibernatable websocket and we are falling through to deferred proxying to
  // potentially allow for hibernation to occur.

  // To begin deferred proxying, we can use this magic `KJ_CO_MAGIC` expression, which fulfills
  // our outer promise for a DeferredProxy<void>, which wraps a promise for the rest of this
  // coroutine.
  KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;

  co_return co_await promise;
}

void WebSocket::accept(jsg::Lock& js) {
  auto& native = *farNative;
  JSG_REQUIRE(!native.state.is<AwaitingConnection>(), TypeError,
      "Websockets obtained from the 'new WebSocket()' constructor cannot call accept");
  JSG_REQUIRE(!native.state.is<Released>(), TypeError,
      "Can't accept() WebSocket that was already used in a response.");

  KJ_IF_SOME(accepted, native.state.tryGet<Accepted>()) {
    JSG_REQUIRE(!accepted.isHibernatable(), TypeError,
        "Can't accept() WebSocket after enabling hibernation.");
    // Technically, this means it's okay to invoke `accept()` once a `new WebSocket()` resolves to
    // an established connection. This is probably okay? It might spare the worker devs a class of
    // errors they do not care care about.
    return;
  }

  internalAccept(js, IoContext::current().getCriticalSection());
}

void WebSocket::internalAccept(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) {
  auto& native = *farNative;
  auto nativeWs = kj::mv(KJ_ASSERT_NONNULL(native.state.tryGet<AwaitingAcceptanceOrCoupling>()).ws);
  native.state.init<Accepted>(kj::mv(nativeWs), native, IoContext::current());
  return startReadLoop(js, kj::mv(cs));
}

WebSocket::Accepted::Accepted(kj::Own<kj::WebSocket> wsParam, Native& native, IoContext& context)
    : ws(kj::mv(wsParam)),
      whenAbortedTask(createAbortTask(native, context)) {
  KJ_IF_SOME(a, context.getActor()) {
    auto& metrics = a.getMetrics();
    metrics.webSocketAccepted();

    // Save the metrics object for the destructor since the IoContext may not be accessible
    // there.
    actorMetrics = kj::addRef(metrics);
  }
}

WebSocket::Accepted::Accepted(Hibernatable wsParam, Native& native, IoContext& context)
    : ws(kj::mv(wsParam)),
      whenAbortedTask(createAbortTask(native, context)) {
  KJ_IF_SOME(a, context.getActor()) {
    auto& metrics = a.getMetrics();
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
  KJ_IF_SOME(a, actorMetrics) {
    a.get()->webSocketClosed();
  }
}

void WebSocket::startReadLoop(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) {
  // If the kj::WebSocket happens to be an AbortableWebSocket (see util/abortable.h), then
  // calling readLoop here could throw synchronously if the canceler has already been tripped.
  // Using kj::evalNow() here let's us capture that and handle correctly.
  //
  // We catch exceptions and return Maybe<Exception> instead since we want to handle the exceptions
  // in awaitIo() below, but we don't want the KJ exception converted to JavaScript before we can
  // examine it.
  kj::Promise<kj::Maybe<kj::Exception>> promise = readLoop(kj::mv(cs));

  auto& context = IoContext::current();

  auto hasLocalPeer = [&]() {
    KJ_IF_SOME(p, peer) {
      if (p->isValid()) {
        return true;
      }
    }
    return false;
  };
  if (!hasLocalPeer()) {
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
  context.addWaitUntil(context.awaitJs(js, context.awaitIoLegacy(js, kj::mv(promise))
      .then(js, [this, thisHandle = JSG_THIS]
                (jsg::Lock& js, kj::Maybe<kj::Exception>&& maybeError) mutable {
    auto& native = *farNative;
    KJ_IF_SOME(e, maybeError) {
      if (!native.closedIncoming && e.getType() == kj::Exception::Type::DISCONNECTED) {
        // Report premature disconnect or cancel as a close event.
        dispatchEventImpl(js, jsg::alloc<CloseEvent>(
            1006, kj::str("WebSocket disconnected without sending Close frame."), false));
        native.closedIncoming = true;
        // If there are no further messages to send, so we can discard the underlying connection.
        tryReleaseNative(js);
      } else {
        native.closedIncoming = true;
        reportError(js, kj::cp(e));
        kj::throwFatalException(kj::mv(e));
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

  auto pendingAutoResponses = autoResponseStatus.pendingAutoResponseDeque.size() -
      autoResponseStatus.queuedAutoResponses;
  autoResponseStatus.queuedAutoResponses = autoResponseStatus.pendingAutoResponseDeque.size();
  outgoingMessages->insert(GatedMessage{kj::mv(maybeOutputLock), kj::mv(msg), pendingAutoResponses});

  ensurePumping(js);
}

void WebSocket::close(
    jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<kj::String> reason) {
  auto& native = *farNative;

  // Handle close before connection is established for websockets obtained through `new WebSocket()`.
  KJ_IF_SOME(pending, native.state.tryGet<AwaitingConnection>()) {
    pending.canceler.cancel(kj::str("Called close before connection was established."));

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

  KJ_IF_SOME(c, code) {
    JSG_REQUIRE(c >= 1000 && c < 5000 && c != 1004 && c != 1005 && c != 1006 && c != 1015,
                 TypeError, "Invalid WebSocket close code: ", c, ".");
  }
  if (reason != kj::none) {
    // The default code of 1005 cannot have a reason, per the standard, so if a reason is specified
    // then there must be a code, too.
    JSG_REQUIRE(code != nullptr, TypeError,
        "If you specify a WebSocket close reason, you must also specify a code.");
  }

  // pendingAutoResponses stores the number of queuedAutoResponses that will be pumped before sending
  // the current GatedMessage, guaranteeing order.
  // queuedAutoResponses stores the total number of auto-response messages that are already in accounted
  // for in previous GatedMessages. This is useful to easily calculate the number of pendingAutoResponses
  // for each new GateMessage.
  auto pendingAutoResponses = autoResponseStatus.pendingAutoResponseDeque.size() -
      autoResponseStatus.queuedAutoResponses;
  autoResponseStatus.queuedAutoResponses = autoResponseStatus.pendingAutoResponseDeque.size();

  outgoingMessages->insert(GatedMessage{
      IoContext::current().waitForOutputLocksIfNecessary(),
      kj::WebSocket::Close {
        // Code 1005 actually translates to sending a close message with no body on the wire.
        static_cast<uint16_t>(code.orDefault(1005)),
        kj::mv(reason).orDefault(nullptr),
      }, pendingAutoResponses
  });

  native.closedOutgoing = true;
  closedOutgoingForHib = true;
  ensurePumping(js);
}

int WebSocket::getReadyState() {
  auto& native = *farNative;
  if ((native.closedIncoming && native.closedOutgoing) || error != kj::none) {
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
      return kj::none;
    }
    KJ_CASE_ONEOF(container, AwaitingAcceptanceOrCoupling) {
      return container.ws->getPreferredExtensions(ctx);
    }
    KJ_CASE_ONEOF(container, Accepted) {
      return container.ws->getPreferredExtensions(ctx);
    }
    KJ_CASE_ONEOF(container, Released) {
      return kj::none;
    }
  }
  return kj::none;
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

kj::Maybe<jsg::JsValue> WebSocket::deserializeAttachment(jsg::Lock& js) {
  return serializedAttachment.map([&](kj::ArrayPtr<byte> attachment)
      -> jsg::JsValue {
    jsg::Deserializer deserializer(js, attachment, kj::none, kj::none,
        jsg::Deserializer::Options {
      .version = 15,
      .readHeader = true,
    });

    return deserializer.readValue(js);
  });
}

void WebSocket::serializeAttachment(jsg::Lock& js, jsg::JsValue attachment) {
  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(js, attachment);
  auto released = serializer.release();
  JSG_REQUIRE(released.data.size() <= MAX_ATTACHMENT_SIZE, Error,
      "A WebSocket 'attachment' cannot be larger than ",  MAX_ATTACHMENT_SIZE, " bytes." \
      "'attachment' was ", released.data.size(), " bytes.");
  serializedAttachment = kj::mv(released.data);
}

void WebSocket::setAutoResponseStatus(kj::Maybe<kj::Date> time,
    kj::Promise<void> autoResponsePromise) {
  autoResponseTimestamp = time;
  autoResponseStatus.ongoingAutoResponse = kj::mv(autoResponsePromise);
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
      return accepted.canceler.wrap(pump(context, *outgoingMessages,
        *accepted.ws, native, autoResponseStatus, observer));
    });

    // TODO(cleanup): We use awaitIoLegacy() here because we don't want this to count as a pending
    //   event if this is a WebSocketPair with the other end being handled in the same isolate.
    //   In that case, the pump can hang if accept() is never called on the other end. Ideally,
    //   this scenario would be handled in-isolate using jsg::Promise, but that would take some
    //   refactoring.
    context.awaitIoLegacy(js, kj::mv(promise))
        .then(js, [this, thisHandle = JSG_THIS](jsg::Lock& js) {
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
        if (awaitingHibernatableRelease()) {
          // TODO(someday): These async races can be pretty complicated, and while it's good to have
          // tests to make sure we're not broken, it would be nice to refactor this code eventually.

          // Hibernatable WebSockets had a subtle race condition where one pump() promise would
          // start right after a previous pump() completed, but before this continuation ran.
          //
          // This race prevented close messages from being sent from inside the webSocketClose()
          // handler because prior to the CLOSE getting sent in the second pump(), the promise
          // continuation following the first pump() would transition us from Accepted to Released,
          // triggering the canceler and cancelling the outgoing CLOSE of the second pump() promise.
          //
          // For a more detailed explanation, see https://github.com/cloudflare/workerd/pull/1535.
          tryReleaseNative(js);
        } else if (native.state.is<Accepted>()) {
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
        reportError(js, jsg::JsValue(exception.getHandle(js)).addRef(js));
      }
    });
  }
}

kj::Promise<void> WebSocket::sendAutoResponse(kj::String message, kj::WebSocket& ws) {
  if (autoResponseStatus.isPumping) {
    autoResponseStatus.pendingAutoResponseDeque.push_back(kj::mv(message));
  } else if (!autoResponseStatus.isClosed){
    auto p = ws.send(message).fork();
    autoResponseStatus.ongoingAutoResponse = p.addBranch();
    co_await p;
    autoResponseStatus.ongoingAutoResponse = kj::READY_NOW;
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
    IoContext& context, OutgoingMessagesMap& outgoingMessages, kj::WebSocket& ws, Native& native,
    AutoResponse& autoResponse, kj::Maybe<kj::Own<WebSocketObserver>>& observer) {
  KJ_ASSERT(!native.isPumping);
  native.isPumping = true;
  autoResponse.isPumping = true;
  KJ_DEFER({
    // We use a KJ_DEFER to set native.isPumping = false to ensure that it happens -- we had a bug
    // in the past where this was handled by the caller of WebSocket::pump() and it allowed for
    // messages to get stuck in `outgoingMessages` until the pump task was restarted.
    native.isPumping = false;

    // Either we were already through all our outgoing messages or we experienced failure/
    // cancellation and cannot send these anyway.
    outgoingMessages.clear();

    autoResponse.isPumping = false;

    if (autoResponse.pendingAutoResponseDeque.size() > 0) {
      autoResponse.pendingAutoResponseDeque.clear();
    }
  });

  // If we have a ongoingAutoResponse, we must co_await it here because there's a ws.send()
  // in progress. Otherwise there can occur ws.send() race problems.
  co_await autoResponse.ongoingAutoResponse;
  autoResponse.ongoingAutoResponse = kj::READY_NOW;

  while (outgoingMessages.size() > 0) {
    GatedMessage gatedMessage = outgoingMessages.release(*outgoingMessages.ordered().begin());
    KJ_IF_SOME(promise, gatedMessage.outputLock) {
      co_await promise;
    }

    auto size = countBytesFromMessage(gatedMessage.message);

    while (gatedMessage.pendingAutoResponses > 0) {
      KJ_ASSERT(autoResponse.pendingAutoResponseDeque.size() >= gatedMessage.pendingAutoResponses);
      auto message = kj::mv(autoResponse.pendingAutoResponseDeque.front());
      autoResponse.pendingAutoResponseDeque.pop_front();
      gatedMessage.pendingAutoResponses--;
      autoResponse.queuedAutoResponses--;
      co_await ws.send(message);
    }

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
        autoResponse.isClosed = true;
        break;
      }
    }

    KJ_IF_SOME(o, observer) {
      o->sentMessage(size);
    }

    KJ_IF_SOME(a, context.getActor()) {
      a.getMetrics().sentWebSocketMessage(size);
    }
  }

  // If there are any auto-responses left to process, we should do it now.
  // We should also check if the last sent message was a close. Shouldn't happen.
  while (autoResponse.pendingAutoResponseDeque.size() > 0 && !autoResponse.isClosed) {
    auto message = kj::mv(autoResponse.pendingAutoResponseDeque.front());
    autoResponse.pendingAutoResponseDeque.pop_front();
    co_await ws.send(message);
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

kj::Array<kj::StringPtr> WebSocket::getHibernatableTags() {
  auto& accepted = JSG_REQUIRE_NONNULL(farNative->state.tryGet<Accepted>(), Error,
      "you must call 'acceptWebSocket()' before attempting to access the tags of a WebSocket.");
  JSG_REQUIRE(accepted.isHibernatable(), Error, "only hibernatable websockets can have tags.");
  return accepted.ws.getHibernatableTags();
}

kj::Promise<kj::Maybe<kj::Exception>> WebSocket::readLoop(
    kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) {
  try {
    // Note that we'll throw if the websocket has enabled hibernation.
    auto& ws = *KJ_REQUIRE_NONNULL(
        KJ_ASSERT_NONNULL(farNative->state.tryGet<Accepted>()).ws.getIfNotHibernatable());
    auto& context = IoContext::current();
    while (true) {
      auto message = co_await ws.receive();

      auto size = countBytesFromMessage(message);
      KJ_IF_SOME(o, observer) {
        o->receivedMessage(size);
      }

      context.getLimitEnforcer().topUpActor();
      KJ_IF_SOME(a, context.getActor()) {
        a.getMetrics().receivedWebSocketMessage(size);
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
            dispatchEventImpl(js,
                jsg::alloc<MessageEvent>(js, js.str(text)));
          }
          KJ_CASE_ONEOF(data, kj::Array<byte>) {
            dispatchEventImpl(js, jsg::alloc<MessageEvent>(js,
                jsg::JsValue(js.arrayBuffer(kj::mv(data)).getHandle(js))));
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
      }, mapAddRef(cs));

      if (!result) co_return kj::none;
    }
    KJ_UNREACHABLE;
  } catch (...) {
    co_return kj::getCaughtExceptionAsKj();
  }
}

jsg::Ref<WebSocketPair> WebSocketPair::constructor() {
  auto pipe = kj::newWebSocketPipe();
  auto pair = jsg::alloc<WebSocketPair>(
      jsg::alloc<WebSocket>(kj::mv(pipe.ends[0])),
      jsg::alloc<WebSocket>(kj::mv(pipe.ends[1])));
  auto first = pair->getFirst();
  auto second = pair->getSecond();

  first->setPeer(second->addWeakRef());
  second->setPeer(first->addWeakRef());
  return kj::mv(pair);
}

jsg::Ref<WebSocketPair::PairIterator> WebSocketPair::entries(jsg::Lock&) {
  return jsg::alloc<PairIterator>(IteratorState {
    .pair = JSG_THIS,
    .index = 0,
   });
}

void WebSocket::reportError(jsg::Lock& js, kj::Exception&& e) {
  reportError(js, js.exceptionToJsValue(kj::cp(e)));
}

void WebSocket::reportError(jsg::Lock& js, jsg::JsRef<jsg::JsValue> err) {
  // If this is the first error, raise the error event.
  if (error == kj::none) {
    auto msg = kj::str(v8::Exception::CreateMessage(js.v8Isolate, err.getHandle(js))->Get());
    error = err.addRef(js);

    dispatchEventImpl(js, jsg::alloc<ErrorEvent>(kj::str("error"),
      ErrorEvent::ErrorEventInit {
        .message = kj::mv(msg),
        .error = kj::mv(err)
      }));

    // After an error we don't allow further send()s. If the receive loop has also ended then we
    // can destroy the connection. Note that we don't set closedOutgoing = true because that flag
    // is specifically to indicate that `close()` has been called, and it causes `send()` to throw
    // an exception complaining specifically that `close()` was called, which would be
    // inappropriate in this case.
    auto& native = *farNative;
    native.outgoingAborted = true;
    if (native.closedIncoming && !native.isPumping) {
      KJ_IF_SOME(pending, native.state.tryGet<AwaitingConnection>()) {
        // Nothing worth canceling if we're reporting an error from the connection establishment
        // continuations.
        pending.canceler.release();
      }

      // We're no longer pumping so let's make sure we release the native connection here.
      native.state.init<Released>();
    }
  }
}

void WebSocket::assertNoError(jsg::Lock& js) {
  KJ_IF_SOME(e, error) {
    js.throwException(e.addRef(js));
  }
}

void WebSocket::setPeer(kj::Own<WeakRef<WebSocket>> other) {
  peer = kj::mv(other);
}

kj::Own<kj::WebSocket> WebSocket::acceptAsHibernatable(kj::Array<kj::StringPtr> tags) {
  KJ_IF_SOME(hibernatable, farNative->state.tryGet<AwaitingAcceptanceOrCoupling>()) {
    // We can only request hibernation if we have not called accept.
    auto ws = kj::mv(hibernatable.ws);
    // We pass a reference to the kj::WebSocket for the api::WebSocket to refer to when calling
    // `send()` or `close()`.
    farNative->state.init<Accepted>(
        Accepted::Hibernatable {
            .ws = *ws,
            .tagsRef = kj::mv(tags) },
        *farNative, IoContext::current());
    return kj::mv(ws);
  }
  JSG_FAIL_REQUIRE(TypeError,
      "Tried to make an api::WebSocket hibernatable when it was in an incompatible state.");
}

void WebSocket::initiateHibernatableRelease(jsg::Lock& js,
    kj::Own<kj::WebSocket> ws,
    kj::Array<kj::String> tags,
    HibernatableReleaseState releaseState) {
  // TODO(soon): We probably want this to be an assert, since this is meant to be called once
  // at the end of a websocket connection>
  KJ_IF_SOME(state, farNative->state.tryGet<Accepted>()) {
    KJ_REQUIRE(state.isHibernatable(),
        "tried to initiate hibernatable release but websocket wasn't hibernatable");
    state.ws.initiateHibernatableRelease(js, kj::mv(ws), kj::mv(tags), releaseState);
    farNative->closedIncoming = true;
  } else {
    KJ_LOG(WARNING, "Unexpected Hibernatable WebSocket state on release", farNative->state);
  }
}

bool WebSocket::awaitingHibernatableError() {
  KJ_IF_SOME(accepted, farNative->state.tryGet<Accepted>()) {
    return (accepted.ws.isAwaitingError());
  }
  return false;
}

bool WebSocket::awaitingHibernatableRelease() {
  KJ_IF_SOME(accepted, farNative->state.tryGet<Accepted>()) {
    return (accepted.ws.isAwaitingRelease());
  }
  return false;
}

bool WebSocket::peerIsAwaitingCoupling() {
  bool answer = false;
  KJ_IF_SOME(p, peer) {
    p->runIfAlive([&answer](WebSocket& ws) {
      answer = ws.farNative->state.is<AwaitingAcceptanceOrCoupling>();
    });
  }
  return answer;
}

WebSocket::HibernationPackage WebSocket::buildPackageForHibernation() {
  // TODO(cleanup): It would be great if we could limit this so only the HibernationManager
  // (or a derived class) could call it.
  return HibernationPackage {
    .url = kj::mv(url),
    .protocol = kj::mv(protocol),
    .extensions = kj::mv(extensions),
    .serializedAttachment = kj::mv(serializedAttachment),
    .maybeTags = kj::none,
    .closedOutgoingConnection = closedOutgoingForHib,
  };
}

WebSocket::Accepted::WrappedWebSocket::WrappedWebSocket(Hibernatable ws) {
  inner.init<Hibernatable>(kj::mv(ws));
}

WebSocket::Accepted::WrappedWebSocket::WrappedWebSocket(kj::Own<kj::WebSocket> ws) {
  inner.init<kj::Own<kj::WebSocket>>(kj::mv(ws));
}

kj::WebSocket* WebSocket::Accepted::WrappedWebSocket::operator->() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(owned, kj::Own<kj::WebSocket>) {
      return owned.get();
    }
    KJ_CASE_ONEOF(hibernatable, Hibernatable) {
      return &hibernatable.ws;
    }
  }
  KJ_UNREACHABLE;
}

kj::WebSocket& WebSocket::Accepted::WrappedWebSocket::operator*() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(owned, kj::Own<kj::WebSocket>) {
      return *owned;
    }
    KJ_CASE_ONEOF(hibernatable, Hibernatable) {
      return hibernatable.ws;
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<kj::Own<kj::WebSocket>&> WebSocket::Accepted::WrappedWebSocket::getIfNotHibernatable() {
  // The implication of getting nullptr is that this websocket is hibernatable. This is useful
  // if the caller only ever expects to get a regular websocket, for example, if they are in
  // any method that should be inaccessible to hibernatable websockets (ex. readLoop).
  return inner.tryGet<kj::Own<kj::WebSocket>>();
}

kj::Maybe<WebSocket::Accepted::Hibernatable&>
WebSocket::Accepted::WrappedWebSocket::getIfHibernatable() {
  return inner.tryGet<Hibernatable>();
}

kj::Array<kj::StringPtr> WebSocket::Accepted::WrappedWebSocket::getHibernatableTags() {
  KJ_SWITCH_ONEOF(KJ_REQUIRE_NONNULL(inner.tryGet<Hibernatable>()).tagsRef) {
    KJ_CASE_ONEOF(ref, kj::Array<kj::StringPtr>) {
      // Tags are still owned by the HibernationManager
      return kj::heapArray<kj::StringPtr>(ref);
    }
    KJ_CASE_ONEOF(arr, kj::Array<kj::String>) {
      // We have the array already, let's copy it and return.
      auto cpy = kj::heapArray<kj::StringPtr>(arr.size());
      for (auto& i: kj::indices(arr)) {
        cpy[i] = kj::StringPtr(arr[i]);
      }
      return cpy;
    }
  }
  KJ_UNREACHABLE;
}

void WebSocket::Accepted::WrappedWebSocket::initiateHibernatableRelease(jsg::Lock& js,
    kj::Own<kj::WebSocket> ws,
    kj::Array<kj::String> tags,
    HibernatableReleaseState state) {
  auto& hibernatable = KJ_REQUIRE_NONNULL(getIfHibernatable());
  hibernatable.releaseState = state;
  // Note that we move the owned kj::WebSocket here.
  hibernatable.attachedForClose = kj::mv(ws);
  hibernatable.tagsRef.init<kj::Array<kj::String>>(kj::mv(tags));
}

bool WebSocket::Accepted::WrappedWebSocket::isAwaitingRelease() {
  KJ_IF_SOME(ws, getIfHibernatable()) {
    return (ws.releaseState != HibernatableReleaseState::NONE);
  }
  return false;
}

bool WebSocket::Accepted::WrappedWebSocket::isAwaitingError() {
  KJ_IF_SOME(ws, getIfHibernatable()) {
    return (ws.releaseState == HibernatableReleaseState::ERROR);
  }
  return false;
}

bool WebSocket::Accepted::isHibernatable() {
  return ws.getIfNotHibernatable() == kj::none;
}

void WebSocketPair::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField(nullptr, sockets[0]);
  tracker.trackField(nullptr, sockets[1]);
}

void WebSocket::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("url", url);
  tracker.trackField("protocol", protocol);
  tracker.trackField("extensions", extensions);
  KJ_IF_SOME(attachment, serializedAttachment) {
    tracker.trackFieldWithSize("attachment", attachment.size());
  }
  tracker.trackFieldWithSize("IoOwn<Native>", sizeof(IoOwn<Native>));
  tracker.trackField("error", error);
  tracker.trackFieldWithSize("IoOwn<OutgoingMessagesMap>", sizeof(IoOwn<OutgoingMessagesMap>));
  tracker.trackField("autoResponseStatus", autoResponseStatus);
}

}  // namespace workerd::api
