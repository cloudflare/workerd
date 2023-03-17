// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-context.h"
#include <workerd/io/hibernation-manager.h>
#include <workerd/util/uuid.h>

namespace workerd {

HibernationManagerImpl::~HibernationManagerImpl() noexcept(false) {
  // Note that the HibernatableWebSocket destructor handles removing any references to itself in
  // `tagToWs`, and even removes the hashmap entry if there are no more entries in the bucket.
  allWs.clear();
  KJ_ASSERT(tagToWs.size() == 0, "tagToWs hashmap wasn't cleared.");
}

void HibernationManagerImpl::acceptWebSocket(
    jsg::Ref<api::WebSocket> ws,
    kj::ArrayPtr<kj::String> tags) {
  // First, we create the HibernatableWebSocket and add it to the collection where it'll stay
  // until it's destroyed.

  JSG_REQUIRE(allWs.size() < ACTIVE_CONNECTION_LIMIT, Error, "only ", ACTIVE_CONNECTION_LIMIT,
      " websockets can be accepted on a single Durable Object instance");

  auto hib = kj::heap<HibernatableWebSocket>(kj::mv(ws), tags, *this);
  HibernatableWebSocket& refToHibernatable = *hib.get();
  allWs.push_front(kj::mv(hib));
  refToHibernatable.node = allWs.begin();

  // If the `tags` array is empty (i.e. user did not provide a tag), we skip the population of the
  // `tagToWs` HashMap below and go straight to initiating the readLoop.

  // It is the caller's responsibility to ensure all elements of `tags` are unique.
  // TODO(cleanup): Maybe we could enforce uniqueness by using an immutable type that
  // can only be constructed if the elements in the collection are distinct, ex. "DistinctArray".
  //
  // We need to add the HibernatableWebSocket to each bucket in `tagToWs` corresponding to its tags.
  //  1. Create the entry if it doesn't exist
  //  2. Fill the TagListItem in the HibernatableWebSocket's tagItems array
  size_t position = 0;
  for (auto tag = tags.begin(); tag < tags.end(); tag++, position++) {
    auto& tagCollection = tagToWs.findOrCreate(*tag, [&tag]() {
      auto item = kj::heap<TagCollection>(
          kj::mv(*tag), kj::heap<kj::List<TagListItem, &TagListItem::link>>());
      return decltype(tagToWs)::Entry {
          item->tag,
          kj::mv(item)
      };
    });
    // This TagListItem sits in the HibernatableWebSocket's tagItems array.
    auto& tagListItem = refToHibernatable.tagItems[position];
    tagListItem.hibWS = refToHibernatable;
    tagListItem.tag = tagCollection->tag.asPtr();

    auto& list = tagCollection->list;
    list->add(tagListItem);
    // We also give the TagListItem a reference to the list it was added to so the
    // HibernatableWebSocket can quickly remove itself from the list without doing a lookup
    // in `tagToWs`.
    tagListItem.list = *list.get();
  }

  // Finally, we initiate the readloop for this HibernatableWebSocket.
  kj::Promise<kj::Maybe<kj::Exception>> readLoopPromise = kj::evalNow([&] {
    return readLoop(refToHibernatable);
  }).then([]() -> kj::Maybe<kj::Exception> { return nullptr; },
          [](kj::Exception&& e) -> kj::Maybe<kj::Exception> { return kj::mv(e); });

  // Give the task to the HibernationManager so it lives long.
  readLoopTasks.add(readLoopPromise.then(
      [&refToHibernatable, this](kj::Maybe<kj::Exception>&& maybeError) -> kj::Promise<void> {
    return handleSocketTermination(refToHibernatable, maybeError);
  }));
}

kj::Vector<jsg::Ref<api::WebSocket>> HibernationManagerImpl::getWebSockets(
    jsg::Lock& js,
    kj::Maybe<kj::StringPtr> maybeTag) {
  kj::Vector<jsg::Ref<api::WebSocket>> matches;
  KJ_IF_MAYBE(tag, maybeTag) {
    KJ_IF_MAYBE(item, tagToWs.find(*tag)) {
      auto& list = *((*item)->list);
      for (auto& entry: list) {
        auto& hibWS = KJ_REQUIRE_NONNULL(entry.hibWS);
        matches.add(hibWS.getActiveOrUnhibernate(js));
      }
    }
  } else {
    // Add all websockets!
    for (auto& hibWS : allWs) {
      matches.add(hibWS->getActiveOrUnhibernate(js));
    }
  }
  return kj::mv(matches);
}

void HibernationManagerImpl::setWebSocketAutoResponse(
    jsg::Ref<api::WebSocketRequestResponsePair> reqResp) {
  autoResponsePair = kj::mv(reqResp);
}

void HibernationManagerImpl::unsetWebSocketAutoResponse() {
  autoResponsePair = nullptr;
}

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> HibernationManagerImpl::getWebSocketAutoResponse() {
  KJ_IF_MAYBE(ar, autoResponsePair) {
    return ar->addRef();
  } else {
    return nullptr;
  }
}

void HibernationManagerImpl::setTimerChannel(TimerChannel& timerChannel) {
  timer = timerChannel;
}

void HibernationManagerImpl::hibernateWebSockets(Worker::Lock& lock) {
  jsg::Lock& js(lock);
  v8::HandleScope handleScope(js.v8Isolate);
  v8::Context::Scope contextScope(lock.getContext());
  for (auto& ws : allWs) {
    KJ_IF_MAYBE(active, ws->activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
      // Transfers ownership of properties from api::WebSocket to HibernatableWebSocket via the
      // HibernationPackage.
      ws->activeOrPackage.init<api::WebSocket::HibernationPackage>(
          active->get()->buildPackageForHibernation());
    }
  }
}

void HibernationManagerImpl::dropHibernatableWebSocket(HibernatableWebSocket& hib) {
  removeFromAllWs(hib);
}

inline void HibernationManagerImpl::removeFromAllWs(HibernatableWebSocket& hib) {
  auto& node = KJ_REQUIRE_NONNULL(hib.node);
  allWs.erase(node);
}

kj::Promise<void> HibernationManagerImpl::handleSocketTermination(
    HibernatableWebSocket& hib, kj::Maybe<kj::Exception>& maybeError) {
  kj::Maybe<kj::Promise<void>> event;
  KJ_IF_MAYBE(error, maybeError) {

    auto websocketId = randomUUID(nullptr);
    webSocketsForEventHandler.insert(kj::str(websocketId), &hib);
    kj::Maybe<api::HibernatableSocketParams> params;
    if (!hib.hasDispatchedClose && (error->getType() == kj::Exception::Type::DISCONNECTED)) {
      // If premature disconnect/cancel, dispatch a close event if we haven't already.
      hib.hasDispatchedClose = true;
      params = api::HibernatableSocketParams(
          1006,
          kj::str("WebSocket disconnected without sending Close frame."),
          false,
          kj::mv(websocketId));
    } else {
      // Otherwise, we need to dispatch an error event!
      params = api::HibernatableSocketParams(kj::mv(*error), kj::mv(websocketId));
    }
    // Dispatch the event.
    auto workerInterface = loopback->getWorker(IoChannelFactory::SubrequestMetadata{});
    event = workerInterface->customEvent(kj::heap<api::HibernatableWebSocketCustomEventImpl>(
        hibernationEventType, readLoopTasks, kj::mv(KJ_REQUIRE_NONNULL(params)), *this))
            .ignoreResult().attach(kj::mv(workerInterface));
  }

  // Returning the event promise will store it in readLoopTasks.
  // After the task completes, we want to drop the websocket since we've closed the connection.
  KJ_IF_MAYBE(promise, event) {
    return kj::mv(*promise).then([&]() {
      dropHibernatableWebSocket(hib);
    });
  } else {
    dropHibernatableWebSocket(hib);
    return kj::READY_NOW;
  }
}

kj::Promise<void> HibernationManagerImpl::readLoop(HibernatableWebSocket& hib) {
  // Like the api::WebSocket readLoop(), but we dispatch different types of events.
  auto& ws = *KJ_REQUIRE_NONNULL(hib.ws);
  while (true) {
    kj::WebSocket::Message message = co_await ws.receive();
    // Note that errors are handled by the callee of `readLoop`, since we throw from `receive()`.

    auto skip = false;

    KJ_IF_MAYBE (reqResp, autoResponsePair) {
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          if (text == (*reqResp)->getRequest()) {
            // If the received message matches the one set for auto-response, we must
            // short-circuit readLoop, store the current timestamp and and automatically respond
            // with the expected response.
            TimerChannel& timerChannel = KJ_REQUIRE_NONNULL(timer);
            // We should have set the timerChannel previously in the hibernation manager.
            // If we haven't, we aren't able to get the current time.
            hib.autoResponseTimestamp = timerChannel.now();
            // We'll store the current timestamp in the HibernatableWebSocket to assure it gets
            // stored even if the WebSocket is currently hibernating. In that scenario, the timestamp
            // value will be loaded into the WebSocket during unhibernation.
            KJ_IF_MAYBE(active, hib.activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
              // If the actor is not hibernated/If the WebSocket is active, we need to update
              // autoResponseTimestamp on the active websocket.
              (*active)->setAutoResponseTimestamp(hib.autoResponseTimestamp);
            }
            ws.send((*reqResp)->getResponse().asArray());
            skip = true;
            // If we've sent an auto response message, we should not unhibernate or deliver the
            // received message to the actor
          }
        }
        KJ_CASE_ONEOF_DEFAULT {}
      }
    }

    if (skip) {
      continue;
    }

    auto websocketId = randomUUID(nullptr);
    webSocketsForEventHandler.insert(kj::str(websocketId), &hib);

    // Build the event params depending on what type of message we got.
    kj::Maybe<api::HibernatableSocketParams> maybeParams;
    KJ_SWITCH_ONEOF(message) {
      KJ_CASE_ONEOF(text, kj::String) {
        maybeParams.emplace(kj::mv(text), kj::mv(websocketId));
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        maybeParams.emplace(kj::mv(data), kj::mv(websocketId));
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        maybeParams.emplace(close.code, kj::mv(close.reason), true, kj::mv(websocketId));
        // We'll dispatch the close event, so let's mark our websocket as having done so to
        // prevent a situation where we dispatch it twice.
        hib.hasDispatchedClose = true;
      }
    }

    auto params = kj::mv(KJ_REQUIRE_NONNULL(maybeParams));
    auto isClose = params.isCloseEvent();
    // Dispatch the event.
    auto workerInterface = loopback->getWorker(IoChannelFactory::SubrequestMetadata{});
    co_await workerInterface->customEvent(
        kj::heap<api::HibernatableWebSocketCustomEventImpl>(
            hibernationEventType, readLoopTasks, kj::mv(params), *this));
    if (isClose) {
      co_return;
    }
  }
}

}; // namespace workerd
