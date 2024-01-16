// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-channels.h"
#include "hibernation-manager.h"
#include <workerd/util/uuid.h>

namespace workerd {

HibernationManagerImpl::HibernatableWebSocket::HibernatableWebSocket(
    jsg::Ref<api::WebSocket> websocket,
    kj::ArrayPtr<kj::String> tags,
    HibernationManagerImpl& manager)
    : tagItems(kj::heapArray<TagListItem>(tags.size())),
      activeOrPackage(kj::mv(websocket)),
      // The `ws` starts off empty because we need to set up our tagging infrastructure before
      // calling api::WebSocket::acceptAsHibernatable(). We will transfer ownership of the
      // kj::WebSocket prior to starting the readLoop.
      ws(kj::none),
      manager(manager) {}

HibernationManagerImpl::HibernatableWebSocket::~HibernatableWebSocket() noexcept(false) {
  // We expect this dtor to be called when we're removing a HibernatableWebSocket
  // from our `allWs` collection in the HibernationManager.

  // This removal is fast because we have direct access to each kj::List, as well as direct
  // access to each TagListItem we want to remove.
  for (auto& item: tagItems) {
    KJ_IF_SOME(list, item.list) {
      // The list reference is non-null, so we still have a valid reference to this
      // TagListItem in the list, which we will now remove.
      list.remove(item);
      if (list.empty()) {
        // Remove the bucket in tagToWs if the tag has no more websockets.
        manager.tagToWs.erase(kj::mv(item.tag));
      }
    }
    item.hibWS = nullptr;
    item.list = nullptr;
  }
}

kj::Array<kj::StringPtr> HibernationManagerImpl::HibernatableWebSocket::getTags() {
  auto tags = kj::heapArray<kj::StringPtr>(tagItems.size());
  for (auto i: kj::indices(tagItems)) {
    tags[i] = tagItems[i].tag;
  }
  return tags;
}

kj::Array<kj::String> HibernationManagerImpl::HibernatableWebSocket::cloneTags() {
  auto tags = kj::heapArray<kj::String>(tagItems.size());
  for (auto i: kj::indices(tagItems)) {
    tags[i] = kj::str(tagItems[i].tag);
  }
  return tags;
}

jsg::Ref<api::WebSocket> HibernationManagerImpl::HibernatableWebSocket::getActiveOrUnhibernate(
    jsg::Lock& js) {
  KJ_IF_SOME(package, activeOrPackage.tryGet<api::WebSocket::HibernationPackage>()) {
    // Recreate our tags array for the api::WebSocket.
    package.maybeTags = getTags();

    // Now that we unhibernated the WebSocket, we can set the last received autoResponse timestamp
    // that was stored in the corresponding HibernatableWebSocket. We also move autoResponsePromise
    // from the hibernation manager to api::websocket to prevent possible ws.send races.
    activeOrPackage.init<jsg::Ref<api::WebSocket>>(
        api::WebSocket::hibernatableFromNative(js, *KJ_REQUIRE_NONNULL(ws), kj::mv(package))
    )->setAutoResponseStatus(autoResponseTimestamp, kj::mv(autoResponsePromise));
    autoResponsePromise = kj::READY_NOW;
  }
  return activeOrPackage.get<jsg::Ref<api::WebSocket>>().addRef();
}

HibernationManagerImpl::HibernationManagerImpl(
    kj::Own<Worker::Actor::Loopback> loopback,
    uint16_t hibernationEventType)
    : loopback(kj::mv(loopback)),
      hibernationEventType(hibernationEventType),
      onDisconnect(DisconnectHandler{}),
      readLoopTasks(onDisconnect) {}

HibernationManagerImpl::~HibernationManagerImpl() noexcept(false) {
  // Note that the HibernatableWebSocket destructor handles removing any references to itself in
  // `tagToWs`, and even removes the hashmap entry if there are no more entries in the bucket.
  allWs.clear();
  KJ_ASSERT(tagToWs.size() == 0, "tagToWs hashmap wasn't cleared.");
}

kj::Own<Worker::Actor::HibernationManager> HibernationManagerImpl::addRef() {
  return kj::addRef(*this);
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

  // Before starting the readLoop, we need to move the kj::Own<kj::WebSocket> from the
  // api::WebSocket into the HibernatableWebSocket and accept the api::WebSocket as "hibernatable".
  refToHibernatable.ws = refToHibernatable.activeOrPackage.get<jsg::Ref<api::WebSocket>>()
      ->acceptAsHibernatable(refToHibernatable.getTags());

  // Finally, we initiate the readloop for this HibernatableWebSocket and
  // give the task to the HibernationManager so it lives long.
  readLoopTasks.add(handleReadLoop(refToHibernatable));
}

kj::Promise<void> HibernationManagerImpl::handleReadLoop(HibernatableWebSocket& refToHibernatable) {
  kj::Maybe<kj::Exception> maybeException;
  try {
    co_await readLoop(refToHibernatable);
  } catch (...) {
    maybeException = kj::getCaughtExceptionAsKj();
  }
  co_await handleSocketTermination(refToHibernatable, maybeException);
}

kj::Vector<jsg::Ref<api::WebSocket>> HibernationManagerImpl::getWebSockets(
    jsg::Lock& js,
    kj::Maybe<kj::StringPtr> maybeTag) {
  kj::Vector<jsg::Ref<api::WebSocket>> matches;
  KJ_IF_SOME(tag, maybeTag) {
    KJ_IF_SOME(item, tagToWs.find(tag)) {
      auto& list = *((item)->list);
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
    kj::Maybe<kj::StringPtr> request, kj::Maybe<kj::StringPtr> response) {
  KJ_IF_SOME(req, request) {
    // If we have a request, we must also have a response. If response is kj::none, we'll throw.
    autoResponsePair->request = kj::str(req);
    autoResponsePair->response = kj::str(KJ_REQUIRE_NONNULL(response));
    return;
  }
  // If we don't have a request, we must unset both request and response.
  autoResponsePair->request = kj::none;
  autoResponsePair->response = kj::none;
}

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> HibernationManagerImpl::getWebSocketAutoResponse() {
  KJ_IF_SOME(req, autoResponsePair->request) {
    // When getting the currently set auto-response pair, if we have a request we must have a response
    // set. If not, we'll throw.
    return api::WebSocketRequestResponsePair::constructor(kj::str(req),
        kj::str(KJ_REQUIRE_NONNULL(autoResponsePair->response)));
  }
  return kj::none;
}

void HibernationManagerImpl::setTimerChannel(TimerChannel& timerChannel) {
  timer = timerChannel;
}

void HibernationManagerImpl::hibernateWebSockets(Worker::Lock& lock) {
  jsg::Lock& js(lock);
  js.withinHandleScope([&] {
    js.enterContextScope(lock.getContext());
    for (auto& ws : allWs) {
      KJ_IF_SOME(active, ws->activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
        // Transfers ownership of properties from api::WebSocket to HibernatableWebSocket via the
        // HibernationPackage.
        ws->activeOrPackage.init<api::WebSocket::HibernationPackage>(
            active.get()->buildPackageForHibernation());
      }
    }
  });
}

void HibernationManagerImpl::setEventTimeout(kj::Maybe<uint32_t> timeoutMs) {
  eventTimeoutMs = timeoutMs;
}

kj::Maybe<uint32_t> HibernationManagerImpl::getEventTimeout() {
  return eventTimeoutMs;
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
  KJ_IF_SOME(error, maybeError) {
    auto websocketId = randomUUID(kj::none);
    webSocketsForEventHandler.insert(kj::str(websocketId), &hib);
    kj::Maybe<api::HibernatableSocketParams> params;
    if (!hib.hasDispatchedClose && (error.getType() == kj::Exception::Type::DISCONNECTED)) {
      // If premature disconnect/cancel, dispatch a close event if we haven't already.
      hib.hasDispatchedClose = true;
      params = api::HibernatableSocketParams(
          1006,
          kj::str("WebSocket disconnected without sending Close frame."),
          false,
          kj::mv(websocketId));
    } else {
      // Otherwise, we need to dispatch an error event!
      params = api::HibernatableSocketParams(kj::mv(error), kj::mv(websocketId));
    }

    KJ_REQUIRE_NONNULL(params).setTimeout(eventTimeoutMs);
    // Dispatch the event.
    auto workerInterface = loopback->getWorker(IoChannelFactory::SubrequestMetadata{});
    event = workerInterface->customEvent(kj::heap<api::HibernatableWebSocketCustomEventImpl>(
        hibernationEventType, readLoopTasks, kj::mv(KJ_REQUIRE_NONNULL(params)), *this))
            .ignoreResult().attach(kj::mv(workerInterface));
  }

  // Returning the event promise will store it in readLoopTasks.
  // After the task completes, we want to drop the websocket since we've closed the connection.
  KJ_IF_SOME(promise, event) {
    co_await promise;
  }

  dropHibernatableWebSocket(hib);
}

kj::Promise<void> HibernationManagerImpl::readLoop(HibernatableWebSocket& hib) {
  // Like the api::WebSocket readLoop(), but we dispatch different types of events.
  auto& ws = *KJ_REQUIRE_NONNULL(hib.ws);
  while (true) {
    kj::WebSocket::Message message = co_await ws.receive();
    // Note that errors are handled by the callee of `readLoop`, since we throw from `receive()`.

    auto skip = false;

    // If we have a request != kj::none, we can compare it the received message. This also implies
    // that we have a response set in autoResponsePair.
    KJ_IF_SOME (req, autoResponsePair->request) {
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          if (text == req) {
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
            KJ_SWITCH_ONEOF(hib.activeOrPackage){
              KJ_CASE_ONEOF(apiWs, jsg::Ref<api::WebSocket>) {
                // If the actor is not hibernated/If the WebSocket is active, we need to update
                // autoResponseTimestamp on the active websocket.
                apiWs->setAutoResponseStatus(hib.autoResponseTimestamp, kj::READY_NOW);
                // Since we had a request set, we must have and response that's sent back using the
                // same websocket here. The sending of response is managed in web-socket to avoid
                // possible racing problems with regular websocket messages.
                co_await apiWs->sendAutoResponse(
                    kj::str(KJ_REQUIRE_NONNULL(autoResponsePair->response).asArray()), ws);
              }
              KJ_CASE_ONEOF(package, api::WebSocket::HibernationPackage) {
                if (!package.closedOutgoingConnection) {
                  // We need to store the autoResponsePromise because we may instantiate an api::websocket
                  // If we do that, we have to provide it with the promise to avoid races. This can
                  // happen if we have a websocket hibernating, that unhibernates and sends a
                  // message while ws.send() for auto-response is also sending.
                  auto p = ws.send(
                      KJ_REQUIRE_NONNULL(autoResponsePair->response).asArray()).fork();
                  hib.autoResponsePromise = p.addBranch();
                  co_await p;
                  hib.autoResponsePromise = kj::READY_NOW;
                }
              }
            }
            // If we've sent an auto response message, we should not unhibernate or deliver the
            // received message to the actor
            skip = true;
          }
        }
        KJ_CASE_ONEOF_DEFAULT {}
      }
    }

    if (skip) {
      continue;
    }

    auto websocketId = randomUUID(kj::none);
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
    params.setTimeout(eventTimeoutMs);
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
