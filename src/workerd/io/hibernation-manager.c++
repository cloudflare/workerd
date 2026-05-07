// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hibernation-manager.h"

#include "io-channels.h"

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
    item.hibWS = kj::none;
    item.list = kj::none;
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
    activeOrPackage
        .init<jsg::Ref<api::WebSocket>>(
            api::WebSocket::hibernatableFromNative(js, *KJ_REQUIRE_NONNULL(ws), kj::mv(package)))
        ->setAutoResponseStatus(autoResponseTimestamp, kj::mv(autoResponsePromise));
    autoResponsePromise = kj::READY_NOW;
  }
  return activeOrPackage.get<jsg::Ref<api::WebSocket>>().addRef();
}

HibernationManagerImpl::HibernationManagerImpl(
    kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType)
    : loopback(kj::mv(loopback)),
      hibernationEventType(hibernationEventType),
      onDisconnect(DisconnectHandler{}),
      readLoopTasks(onDisconnect) {}

HibernationManagerImpl::~HibernationManagerImpl() noexcept(false) {
  // Drop our outstanding tasks, the `readLoopTasks` have weak references to the
  // `HibernatableWebSockets` in `allWs`, and since we're about to drop all of those WebSockets,
  // we can't allow any more events to be delivered.
  readLoopTasks.clear();

  // Note that the HibernatableWebSocket destructor handles removing any references to itself in
  // `tagToWs`, and even removes the hashmap entry if there are no more entries in the bucket.
  allWs.clear();
  KJ_ASSERT(tagToWs.size() == 0, "tagToWs hashmap wasn't cleared.");
}

kj::Own<Worker::Actor::HibernationManager> HibernationManagerImpl::addRef() {
  return kj::addRef(*this);
}

void HibernationManagerImpl::acceptWebSocket(
    jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) {
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
      return decltype(tagToWs)::Entry{item->tag, kj::mv(item)};
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
  refToHibernatable.ws =
      refToHibernatable.activeOrPackage.get<jsg::Ref<api::WebSocket>>()->acceptAsHibernatable(
          refToHibernatable.getTags());

  // Finally, we initiate the readloop for this HibernatableWebSocket and
  // give the task to the HibernationManager so it lives long.
  readLoopTasks.add(handleReadLoop(refToHibernatable).catch_([](kj::Exception&& e) {
    if (isInterestingException(e)) {
      LOG_EXCEPTION_IF_INTERNAL("HibernationManagerImpl::handleReadLoop", e);
    }
  }));
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
    jsg::Lock& js, kj::Maybe<kj::StringPtr> maybeTag) {
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
    for (auto& hibWS: allWs) {
      matches.add(hibWS->getActiveOrUnhibernate(js));
    }
  }
  return kj::mv(matches);
}

void HibernationManagerImpl::setWebSocketAutoResponse(
    kj::Maybe<api::WebSocketDataMessage> request, kj::Maybe<api::WebSocketDataMessage> response) {
  KJ_IF_SOME(req, request) {
    auto& resp = KJ_REQUIRE_NONNULL(response);
    autoResponsePair->pair = AutoRequestResponsePair::Pair{kj::mv(req), kj::mv(resp)};
    return;
  }
  autoResponsePair->pair = kj::none;
}

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> HibernationManagerImpl::
    getWebSocketAutoResponse(jsg::Lock& js) {
  KJ_IF_SOME(pair, autoResponsePair->pair) {
    return js.alloc<api::WebSocketRequestResponsePair>(
        pair.request.asPtr().toOwned(), pair.response.asPtr().toOwned());
  }
  return kj::none;
}

void HibernationManagerImpl::setTimerChannel(TimerChannel& timerChannel) {
  timer = timerChannel;
}

void HibernationManagerImpl::hibernateWebSockets(Worker::Lock& lock) {
  JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
    for (auto& ws: allWs) {
      KJ_IF_SOME(active, ws->activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
        // Transfers ownership of properties from api::WebSocket to HibernatableWebSocket via the
        // HibernationPackage.
        ws->activeOrPackage.init<api::WebSocket::HibernationPackage>(
            active.get()->buildPackageForHibernation());
      } else {
      }  // Here to quash compiler warning
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
      params = api::HibernatableSocketParams(1006,
          kj::str("WebSocket disconnected without sending Close frame."), false,
          kj::mv(websocketId));
    } else {
      // Otherwise, we need to dispatch an error event!
      params = api::HibernatableSocketParams(kj::mv(error), kj::mv(websocketId));
    }

    KJ_REQUIRE_NONNULL(params).setTimeout(eventTimeoutMs);
    // Dispatch the event.
    auto workerInterface = loopback->getWorker(IoChannelFactory::SubrequestMetadata{});
    event = workerInterface
                ->customEvent(kj::heap<api::HibernatableWebSocketCustomEvent>(
                    hibernationEventType, kj::mv(KJ_REQUIRE_NONNULL(params)), *this))
                .ignoreResult()
                .attach(kj::mv(workerInterface));
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

    KJ_IF_SOME(pair, autoResponsePair->pair) {
      bool matched = false;
      if (pair.request.isText()) {
        KJ_IF_SOME(text, message.tryGet<kj::String>()) {
          matched = (text == pair.request);
        }
      } else if (pair.request.isBinary()) {
        KJ_IF_SOME(data, message.tryGet<kj::Array<kj::byte>>()) {
          matched = (data == pair.request);
        }
      }

      if (matched) {
        // Short-circuit readLoop: store the current timestamp and automatically respond
        // with the expected response instead of dispatching to the actor.
        TimerChannel& timerChannel = KJ_REQUIRE_NONNULL(timer);
        // Count as a new IO event so autoResponseTimestamp is accurate.
        timerChannel.syncTime();
        // Store the timestamp on the HibernatableWebSocket so it survives hibernation.
        // During unhibernation, this value is loaded into the new api::WebSocket.
        hib.autoResponseTimestamp = timerChannel.now();

        KJ_SWITCH_ONEOF(hib.activeOrPackage) {
          KJ_CASE_ONEOF(apiWs, jsg::Ref<api::WebSocket>) {
            apiWs->setAutoResponseStatus(hib.autoResponseTimestamp, kj::READY_NOW);
            // Response sending is managed in web-socket to avoid racing with regular messages.
            co_await apiWs->sendAutoResponse(pair.response.asPtr().toOwned(), ws);
          }
          KJ_CASE_ONEOF(package, api::WebSocket::HibernationPackage) {
            if (!package.closedOutgoingConnection) {
              // Store the autoResponsePromise so that if the WebSocket unhibernates mid-send,
              // the new api::WebSocket can await it to avoid send races.
              auto p = pair.response.sendVia(ws).fork();
              hib.autoResponsePromise = p.addBranch();
              co_await p;
              hib.autoResponsePromise = kj::READY_NOW;
            }
          }
        }
        // Do not unhibernate or deliver the message to the actor.
        skip = true;
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
    co_await workerInterface->customEvent(kj::heap<api::HibernatableWebSocketCustomEvent>(
        hibernationEventType, kj::mv(params), *this));
    if (isClose) {
      co_return;
    }
  }
}

};  // namespace workerd
