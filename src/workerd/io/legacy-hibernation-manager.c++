// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "legacy-hibernation-manager.h"

#include "io-channels.h"
#include "io-context.h"

#include <workerd/util/uuid.h>

#include <kj/common.h>

namespace workerd {

LegacyHibernationManagerImpl::EventRegistry& LegacyHibernationManagerImpl::getEventRegistry() {
  static const kj::EventLoopLocal<EventRegistry> registry;
  return *registry;
}

LegacyHibernationManagerImpl::HibernatableWebSocket::HibernatableWebSocket(
    jsg::Ref<api::WebSocket> websocket,
    kj::ArrayPtr<kj::String> tags,
    LegacyHibernationManagerImpl& manager)
    : tagItems(kj::heapArray<TagListItem>(tags.size())),
      activeOrPackage(kj::mv(websocket)),
      // The `ws` starts off empty because we need to set up our tagging infrastructure before
      // calling api::WebSocket::acceptAsHibernatable(). We will share ownership of the
      // kj::WebSocket before starting the readLoop.
      ws(kj::none),
      manager(manager) {}

LegacyHibernationManagerImpl::HibernatableWebSocket::HibernatableWebSocket(
    api::WebSocket::HibernationPackage package,
    size_t tagCount,
    LegacyHibernationManagerImpl& manager)
    : tagItems(kj::heapArray<TagListItem>(tagCount)),
      activeOrPackage(kj::mv(package)),
      ws(kj::none),
      manager(manager) {}

LegacyHibernationManagerImpl::HibernatableWebSocket::~HibernatableWebSocket() noexcept(false) {
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
        // Remove the bucket in tagToWs if the tag has no more websockets. Release this item's
        // non-owning view before erasing the collection that owns the tag.
        auto tag = kj::str(item.tag);
        item.tag = nullptr;
        manager.tagToWs.erase(tag);
      }
    }
    item.hibWS = kj::none;
    item.list = kj::none;
  }
}

class LegacyHibernationManagerImpl::LoopbackWaiterAdapter final: public LoopbackWaiter {
 public:
  LoopbackWaiterAdapter(kj::PromiseFulfiller<kj::Own<WorkerInterface>>& fulfiller,
      LoopbackWaiterList& waiters,
      IoChannelFactory::SubrequestMetadata metadata)
      : LoopbackWaiter{fulfiller, kj::mv(metadata)},
        waiters(waiters) {
    waiters.add(*this);
  }

  ~LoopbackWaiterAdapter() noexcept(false) {
    unlink();
  }

  void moveTo(LoopbackWaiterList& destination) {
    unlink();
    waiters = destination;
    destination.add(*this);
  }

  void unlink() {
    KJ_IF_SOME(w, waiters) {
      if (link.isLinked()) w.remove(*this);
    }
    waiters = kj::none;
  }

 private:
  kj::Maybe<LoopbackWaiterList&> waiters;
};

kj::Array<kj::String> LegacyHibernationManagerImpl::HibernatableWebSocket::getTags() {
  auto tags = kj::heapArray<kj::String>(tagItems.size());
  for (auto i: kj::indices(tagItems)) {
    tags[i] = kj::str(tagItems[i].tag);
  }
  return tags;
}

kj::Promise<void> LegacyHibernationManagerImpl::HibernatableWebSocket::branchWriteBarrier() {
  KJ_IF_SOME(promise, maybeWriteBarrier) {
    return promise.addBranch();
  }
  return kj::READY_NOW;
}

void LegacyHibernationManagerImpl::HibernatableWebSocket::wakeReceiver() {
  KJ_IF_SOME(fulfiller, receiverWaiter) {
    auto owned = kj::mv(fulfiller);
    receiverWaiter = kj::none;
    owned->fulfill();
  }
}

void LegacyHibernationManagerImpl::HibernatableWebSocket::wakeDelivery() {
  KJ_IF_SOME(fulfiller, deliveryWaiter) {
    auto owned = kj::mv(fulfiller);
    deliveryWaiter = kj::none;
    owned->fulfill();
  }
}

void LegacyHibernationManagerImpl::HibernatableWebSocket::updateCountWaiters() {
  kj::Vector<CountWaiter> remaining;
  for (auto& waiter: countWaiters) {
    if (receivedCount >= waiter.count) {
      waiter.fulfiller->fulfill();
    } else KJ_IF_SOME(error, receiveError) {
      waiter.fulfiller->reject(error.clone());
    } else {
      remaining.add(kj::mv(waiter));
    }
  }
  countWaiters = kj::mv(remaining);
}

kj::Promise<void> LegacyHibernationManagerImpl::HibernatableWebSocket::receiveLoop() {
  auto& socket = *KJ_REQUIRE_NONNULL(ws);
  try {
    while (true) {
      // Stay one message ahead of delivery, which keeps the client's flow control as tight as
      // receiving on demand would. While events are held, take everything, so that a migration
      // can carry every message the client has sent.
      while (manager.holdCount == 0 && !pendingMessages.empty()) {
        auto paf = kj::newPromiseAndFulfiller<void>();
        receiverWaiter = kj::mv(paf.fulfiller);
        co_await paf.promise;
      }
      auto message = co_await socket.receive();
      bool isClose = message.is<kj::WebSocket::Close>();
      ++receivedCount;
      pendingMessages.push_back(kj::mv(message));
      updateCountWaiters();
      wakeDelivery();
      if (isClose) {
        // Delivering the Close ends the connection, so there is nothing more to receive.
        co_return;
      }
    }
  } catch (...) {
    receiveError = kj::getCaughtExceptionAsKj();
  }
  updateCountWaiters();
  wakeDelivery();
}

jsg::Ref<api::WebSocket> LegacyHibernationManagerImpl::HibernatableWebSocket::
    getActiveOrUnhibernate(jsg::Lock& js) {
  KJ_IF_SOME(package, activeOrPackage.tryGet<api::WebSocket::HibernationPackage>()) {
    // Recreate our tags array for the api::WebSocket.
    package.maybeTags = getTags();

    // Share the previous send with api::WebSocket while retaining the fork in case the socket
    // hibernates again before it settles.
    activeOrPackage
        .init<jsg::Ref<api::WebSocket>>(api::WebSocket::hibernatableFromNative(
            js, KJ_REQUIRE_NONNULL(ws).addRef(), kj::mv(package)))
        ->setAutoResponseStatus(autoResponseTimestamp, branchWriteBarrier());
  }
  return activeOrPackage.get<jsg::Ref<api::WebSocket>>().addRef();
}

LegacyHibernationManagerImpl::LegacyHibernationManagerImpl(
    kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType)
    : LegacyHibernationManagerImpl(hibernationEventType) {
  this->loopback = kj::mv(loopback);
}

LegacyHibernationManagerImpl::LegacyHibernationManagerImpl(uint16_t hibernationEventType)
    : hibernationEventType(hibernationEventType),
      onDisconnect(DisconnectHandler{}),
      readLoopTasks(onDisconnect) {}

LegacyHibernationManagerImpl::~LegacyHibernationManagerImpl() noexcept(false) {
  // Drop our outstanding tasks, the `readLoopTasks` have weak references to the
  // `HibernatableWebSockets` in `allWs`, and since we're about to drop all of those WebSockets,
  // we can't allow any more events to be delivered.
  //
  // Cancelling those tasks also deregisters the events they were delivering, which is why
  // `registeredEventCount` is normally zero below.
  readLoopTasks.clear();

  while (!loopbackWaiters.empty()) {
    auto& waiter = static_cast<LoopbackWaiterAdapter&>(loopbackWaiters.front());
    waiter.fulfiller.reject(KJ_EXCEPTION(
        DISCONNECTED, "hibernation manager destroyed while waiting for its replacement loopback"));
    waiter.unlink();
  }

  if (registeredEventCount > 0) {
    // The registry holds non-owning pointers, so anything still naming this manager has to go: a
    // surviving entry would route the next event with that ID into freed memory.
    getEventRegistry().eraseAll(
        [this](kj::StringPtr, RegisteredEvent& registered) { return registered.manager == this; });
    registeredEventCount = 0;
  }

  // Note that the HibernatableWebSocket destructor handles removing any references to itself in
  // `tagToWs`, and even removes the hashmap entry if there are no more entries in the bucket.
  allWs.clear();
  KJ_DTOR_ASSERT(tagToWs.size() == 0, "tagToWs hashmap wasn't cleared.");
}

void LegacyHibernationManagerImpl::acceptWebSocket(
    jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) {
  // First, we create the HibernatableWebSocket and add it to the collection where it'll stay
  // until it's destroyed.

  JSG_REQUIRE(allWs.size() < ACTIVE_CONNECTION_LIMIT, Error, "only ", ACTIVE_CONNECTION_LIMIT,
      " websockets can be accepted on a single Durable Object instance");

  auto hib = kj::heap<HibernatableWebSocket>(kj::mv(ws), tags, *this);
  HibernatableWebSocket& refToHibernatable = *hib.get();

  // TODO(mar): Improve accept span context capturing — route snapshotted user span context
  // to serialization point instead of capturing only the invocation root span here.
  auto invCtx = IoContext::current().getInvocationSpanContext();
  refToHibernatable.userSpanContext = tracing::SpanContext(invCtx.getTraceId(), invCtx.getSpanId());

  allWs.push_front(kj::mv(hib));
  refToHibernatable.node = allWs.begin();

  insertTags(refToHibernatable, tags);

  // Before starting the readLoop, share ownership of the kj::WebSocket with the
  // HibernatableWebSocket and accept the api::WebSocket as hibernatable.
  refToHibernatable.ws =
      refToHibernatable.activeOrPackage.get<jsg::Ref<api::WebSocket>>()->acceptAsHibernatable(
          refToHibernatable.getTags());

  startReadLoop(refToHibernatable);
}

void LegacyHibernationManagerImpl::insertTags(
    HibernatableWebSocket& hib, kj::ArrayPtr<kj::String> tags) {
  // If the `tags` array is empty (i.e. user did not provide a tag), we skip the population of the
  // `tagToWs` HashMap.

  // It is the caller's responsibility to ensure all elements of `tags` are unique.
  // TODO(cleanup): Maybe we could enforce uniqueness by using an immutable type that
  // can only be constructed if the elements in the collection are distinct, ex. "DistinctArray".
  //
  // We need to add the HibernatableWebSocket to each bucket in `tagToWs` corresponding to its tags.
  //  1. Create the entry if it doesn't exist
  //  2. Fill the TagListItem in the HibernatableWebSocket's tagItems array
  KJ_ASSERT(tags.size() == hib.tagItems.size());
  size_t position = 0;
  for (auto tag = tags.begin(); tag < tags.end(); tag++, position++) {
    auto& tagCollection = tagToWs.findOrCreate(*tag, [&tag]() {
      auto item = kj::heap<TagCollection>(
          kj::mv(*tag), kj::heap<kj::List<TagListItem, &TagListItem::link>>());
      return decltype(tagToWs)::Entry{kj::str(item->tag), kj::mv(item)};
    });
    // This TagListItem sits in the HibernatableWebSocket's tagItems array.
    auto& tagListItem = hib.tagItems[position];
    tagListItem.hibWS = hib;
    tagListItem.tag = tagCollection->tag.asPtr();

    auto& list = tagCollection->list;
    list->add(tagListItem);
    // We also give the TagListItem a reference to the list it was added to so the
    // HibernatableWebSocket can quickly remove itself from the list without doing a lookup
    // in `tagToWs`.
    tagListItem.list = *list.get();
  }
}

void LegacyHibernationManagerImpl::startReadLoop(HibernatableWebSocket& hib) {
  // The receiver reads from the WebSocket and the read loop delivers what it read. The task lives
  // in the HibernatableWebSocket, so it ends when the WebSocket is removed.
  hib.receiverTask = hib.receiveLoop().eagerlyEvaluate(nullptr);

  // Give the read loop to the HibernationManager so it lives long.
  readLoopTasks.add(handleReadLoop(hib).catch_([](kj::Exception&& e) {
    if (isInterestingException(e)) {
      LOG_EXCEPTION_IF_INTERNAL("LegacyHibernationManagerImpl::handleReadLoop", e);
    }
  }));
}

kj::Promise<void> LegacyHibernationManagerImpl::handleReadLoop(
    HibernatableWebSocket& refToHibernatable) {
  kj::Maybe<kj::Exception> maybeException;
  try {
    co_await readLoop(refToHibernatable);
  } catch (...) {
    maybeException = kj::getCaughtExceptionAsKj();
  }
  co_await handleSocketTermination(refToHibernatable, maybeException);
}

kj::Vector<jsg::Ref<api::WebSocket>> LegacyHibernationManagerImpl::getWebSockets(
    jsg::Lock& js, kj::Maybe<kj::StringPtr> maybeTag) {
  kj::Vector<jsg::Ref<api::WebSocket>> matches;
  KJ_IF_SOME(tag, maybeTag) {
    KJ_IF_SOME(item, tagToWs.find(tag)) {
      auto& list = *((item)->list);
      for (auto& entry: list) {
        auto& hibWS = KJ_REQUIRE_NONNULL(entry.hibWS);
        // A WebSocket moved to another process lingers until its read loop ends.
        if (hibWS.exported) continue;
        matches.add(hibWS.getActiveOrUnhibernate(js));
      }
    }
  } else {
    // Add all websockets!
    for (auto& hibWS: allWs) {
      if (hibWS->exported) continue;
      matches.add(hibWS->getActiveOrUnhibernate(js));
    }
  }
  return kj::mv(matches);
}

uint64_t LegacyHibernationManagerImpl::getWebSocketCount() const {
  // A WebSocket moved to another process lingers until its read loop ends, but is no longer ours.
  return allWs.size() - exportedCount;
}

void LegacyHibernationManagerImpl::setWebSocketAutoResponse(
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

kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> LegacyHibernationManagerImpl::
    getWebSocketAutoResponse(jsg::Lock& js) {
  KJ_IF_SOME(req, autoResponsePair->request) {
    // When getting the currently set auto-response pair, if we have a request we must have a response
    // set. If not, we'll throw.
    return api::WebSocketRequestResponsePair::constructor(
        js, kj::str(req), kj::str(KJ_REQUIRE_NONNULL(autoResponsePair->response)));
  }
  return kj::none;
}

void LegacyHibernationManagerImpl::setLoopback(kj::Own<Worker::Actor::Loopback> loopback) {
  // A loopback arriving completes any handoff in progress: the parked one belongs to a generation
  // that is gone. Clearing the generation makes dropping that handoff's handle a no-op.
  handoffLoopback = kj::none;
  loopbackHandoffGeneration = 0;
  this->loopback = loopback->addRef();

  // Isolate this batch because fulfilling it can re-enter the manager and add more waiters.
  LoopbackWaiterList waiters;
  while (!loopbackWaiters.empty()) {
    static_cast<LoopbackWaiterAdapter&>(loopbackWaiters.front()).moveTo(waiters);
  }
  while (!waiters.empty()) {
    auto& waiter = static_cast<LoopbackWaiterAdapter&>(waiters.front());
    // Hand a failure to the waiter it belongs to rather than letting it escape: we run from the
    // actor's constructor, where a throw would fail the construction and abandon the remaining
    // waiters with only a "fulfiller destroyed" exception to show for it.
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      waiter.fulfiller.fulfill(loopback->getWorker(kj::mv(waiter.metadata)));
    })) {
      waiter.fulfiller.reject(kj::mv(exception));
    }
    waiter.unlink();
  }
}

// Ends a loopback handoff when dropped. Holds a reference to the manager, which a code update moves
// between actor generations, so the end of the handoff reaches it wherever it ended up.
class LoopbackHandoff {
 public:
  LoopbackHandoff(kj::Rc<LegacyHibernationManagerImpl> manager, uint64_t generation)
      : manager(kj::mv(manager)),
        generation(generation) {}
  ~LoopbackHandoff() noexcept(false) {
    manager->cancelLoopbackHandoff(generation);
  }
  KJ_DISALLOW_COPY_AND_MOVE(LoopbackHandoff);

 private:
  kj::Rc<LegacyHibernationManagerImpl> manager;
  uint64_t generation;
};

kj::Own<void> LegacyHibernationManagerImpl::beginLoopbackHandoff() {
  KJ_REQUIRE(loopbackHandoffGeneration == 0, "a loopback handoff is already in progress");
  loopbackHandoffGeneration = nextLoopbackHandoffGeneration++;
  handoffLoopback = kj::mv(loopback);
  return kj::heap<LoopbackHandoff>(addRefToThis(), loopbackHandoffGeneration);
}

void LegacyHibernationManagerImpl::cancelLoopbackHandoff(uint64_t generation) {
  if (generation != loopbackHandoffGeneration) {
    // This handle's handoff already ended, either because a replacement attached or because a
    // later handoff superseded it. Restoring now would strand whatever is parked for the handoff
    // currently in progress.
    return;
  }
  loopbackHandoffGeneration = 0;
  KJ_IF_SOME(previous, handoffLoopback) {
    setLoopback(kj::mv(previous));
  }
}

void LegacyHibernationManagerImpl::setTimerChannel(TimerChannel& timerChannel) {
  timer = timerChannel;
}

void LegacyHibernationManagerImpl::setOwningActor(Worker::Actor& actor) {
  owningActor = actor.getWeakRef();
  owningActorId = actor.cloneId();
  owningHolderToken = actor.getHolderToken();
}

kj::Maybe<Worker::Actor&> LegacyHibernationManagerImpl::getOwningActor() {
  KJ_IF_SOME(weak, owningActor) {
    return weak->tryGet();
  }
  return kj::none;
}

kj::Maybe<const Worker::Actor::Id&> LegacyHibernationManagerImpl::getOwningActorId() {
  return owningActorId;
}

kj::Maybe<uint64_t> LegacyHibernationManagerImpl::getOwningHolderToken() {
  return owningHolderToken;
}

void LegacyHibernationManagerImpl::forgetOwningHolder() {
  owningHolderToken = kj::none;
}

void LegacyHibernationManagerImpl::hibernateWebSockets(Worker::Lock& lock) {
  JSG_WITHIN_CONTEXT_SCOPE(lock, lock.getContext(), [&](jsg::Lock& js) {
    for (auto& ws: allWs) {
      KJ_IF_SOME(active, ws->activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
        // Transfers ownership of properties from api::WebSocket to HibernatableWebSocket via the
        // HibernationPackage.
        auto package = active.get()->buildPackageForHibernation();
        if (package.maybePumpCompletion != kj::none) {
          auto completion = kj::mv(KJ_ASSERT_NONNULL(package.maybePumpCompletion));
          package.maybePumpCompletion = kj::none;
          ws->maybeWriteBarrier = kj::mv(completion);
        }
        ws->activeOrPackage.init<api::WebSocket::HibernationPackage>(kj::mv(package));
      } else {
      }  // Here to quash compiler warning
    }
  });
}

void LegacyHibernationManagerImpl::setEventTimeout(kj::Maybe<uint32_t> timeoutMs) {
  eventTimeoutMs = kj::mv(timeoutMs);
}

kj::Maybe<uint32_t> LegacyHibernationManagerImpl::getEventTimeout() {
  return eventTimeoutMs;
}

kj::Maybe<kj::Rc<Worker::Actor::HibernationManager>> LegacyHibernationManagerImpl::
    findManagerForEvent(kj::StringPtr websocketId) {
  KJ_IF_SOME(registered, getEventRegistry().find(websocketId)) {
    return registered.manager->addRefToThis();
  }
  return kj::none;
}

LegacyHibernationManagerImpl::HibernatableWebSocket& LegacyHibernationManagerImpl::
    takeWebSocketForEvent(kj::StringPtr websocketId) {
  auto& registry = getEventRegistry();
  auto& entry = KJ_REQUIRE_NONNULL(registry.findEntry(websocketId),
      "hibernatable WebSocket event is not registered", websocketId);
  auto& registered = entry.value;

  // Nothing about the ID comes from the actor claiming the socket, so the manager holding it has
  // to be this actor's own. Generations on different script versions are different isolates, so a
  // stray claim would hand an actor a jsg::Ref minted in another one.
  //
  // Both checks are needed. The ID outlives the owning actor, so it still rejects an unrelated
  // actor once a code update has destroyed the owner. The instance check rejects an actor that is
  // live alongside the owner and shares its ID, which the ID alone would admit.
  KJ_IF_SOME(claimant, IoContext::current().getActor()) {
    KJ_IF_SOME(ownerId, registered.manager->getOwningActorId()) {
      KJ_REQUIRE(Worker::Actor::idsEqual(ownerId, claimant.getId()),
          "hibernatable WebSocket event ID names a socket owned by a different actor");
    }
    KJ_IF_SOME(owner, registered.manager->getOwningActor()) {
      KJ_REQUIRE(&owner == &claimant,
          "hibernatable WebSocket event ID names a socket owned by a different live actor");
    }
  }

  --registered.manager->registeredEventCount;
  auto& webSocket = *registered.webSocket;
  registry.erase(entry);
  return webSocket;
}

void LegacyHibernationManagerImpl::registerEventWebSocket(
    kj::String websocketId, HibernatableWebSocket& hib) {
  auto& registry = getEventRegistry();
  KJ_ASSERT(registry.find(websocketId) == kj::none, "duplicate hibernatable WebSocket event ID",
      websocketId);
  registry.insert(kj::mv(websocketId), RegisteredEvent{this, &hib});
  ++registeredEventCount;
}

void LegacyHibernationManagerImpl::cancelEvent(kj::StringPtr websocketId) {
  auto& registry = getEventRegistry();
  KJ_IF_SOME(entry, registry.findEntry(websocketId)) {
    KJ_ASSERT(entry.value.manager == this,
        "hibernatable WebSocket event registered to a different manager", websocketId);
    --registeredEventCount;
    registry.erase(entry);
  }
}

void LegacyHibernationManagerImpl::dropHibernatableWebSocket(HibernatableWebSocket& hib) {
  if (hib.exported) {
    --exportedCount;
  }
  removeFromAllWs(hib);
}

inline void LegacyHibernationManagerImpl::removeFromAllWs(HibernatableWebSocket& hib) {
  auto& node = KJ_REQUIRE_NONNULL(hib.node);
  allWs.erase(node);
}

kj::Promise<void> LegacyHibernationManagerImpl::handleSocketTermination(
    HibernatableWebSocket& hib, kj::Maybe<kj::Exception>& maybeError) {
  // A failed termination event must not leave a disconnected socket registered.
  kj::String eventWebSocketId;
  KJ_DEFER({
    if (eventWebSocketId.size() > 0) {
      cancelEvent(eventWebSocketId);
    }
    dropHibernatableWebSocket(hib);
  });

  kj::Maybe<kj::Promise<void>> event;
  KJ_IF_SOME(error, maybeError) {
    auto websocketId = randomUUID(kj::none);
    eventWebSocketId = kj::str(websocketId);
    registerEventWebSocket(kj::str(websocketId), hib);
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
    auto workerInterface = getWorkerForEvent(hib);
    event = workerInterface
                ->customEvent(kj::rc<api::HibernatableWebSocketCustomEvent>(
                    hibernationEventType, kj::mv(KJ_REQUIRE_NONNULL(params)))
                                  .toOwn())
                .ignoreResult()
                .attach(kj::mv(workerInterface));
  }

  // Returning the event promise will store it in readLoopTasks.
  // After the task completes, we want to drop the websocket since we've closed the connection.
  KJ_IF_SOME(promise, event) {
    co_await promise;
  }
}

kj::Own<WorkerInterface> LegacyHibernationManagerImpl::getWorkerForEvent(
    HibernatableWebSocket& hib) {
  SpanParent userSpanParent = SpanParent(nullptr);
  KJ_IF_SOME(ctx, hib.userSpanContext) {
    userSpanParent = SpanParent::fromSpanContext(tracing::SpanContext::clone(ctx));
  }
  auto metadata = IoChannelFactory::SubrequestMetadata{
    .userSpanParent = kj::mv(userSpanParent),
    .reresolveActorPipeline =
        getOwningActor() == kj::none ? ReresolveActorPipeline::YES : ReresolveActorPipeline::NO,
  };
  KJ_IF_SOME(l, loopback) {
    // Hold a reference across the call: getWorker() can construct the actor, which hands this
    // manager its own loopback, destroying the one whose getWorker() is still on the stack.
    auto owned = l->addRef();
    return owned->getWorker(kj::mv(metadata));
  }

  kj::Promise<kj::Own<WorkerInterface>> worker =
      kj::newAdaptedPromise<kj::Own<WorkerInterface>, LoopbackWaiterAdapter>(
          loopbackWaiters, kj::mv(metadata));

  // Don't wait for the replacement loopback indefinitely.
  KJ_IF_SOME(t, timer) {
    worker = worker.exclusiveJoin(
        t.afterLimitTimeout(LOOPBACK_HANDOFF_TIMEOUT).then([]() -> kj::Own<WorkerInterface> {
      KJ_FAIL_REQUIRE("hibernatable WebSocket event gave up waiting for the replacement actor's "
                      "loopback during a code-update handoff");
    }));
  }

  return newPromisedWorkerInterface(kj::mv(worker));
}

kj::Promise<void> LegacyHibernationManagerImpl::readLoop(HibernatableWebSocket& hib) {
  // Like the api::WebSocket readLoop(), but we dispatch different types of events.
  while (true) {
    auto maybeMessage = co_await nextMessage(hib);
    if (maybeMessage == kj::none) {
      // A migration moved this WebSocket to another process.
      co_return;
    }
    kj::WebSocket::Message message = kj::mv(KJ_ASSERT_NONNULL(maybeMessage));
    // Note that errors are handled by the callee of `readLoop`, since nextMessage() rethrows the
    // error that ended receiving.

    auto skip = false;

    // If we have a request != kj::none, we can compare it the received message. This also implies
    // that we have a response set in autoResponsePair.
    KJ_IF_SOME(req, autoResponsePair->request) {
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          if (text == req) {
            // If the received message matches the one set for auto-response, we must
            // short-circuit readLoop, store the current timestamp and and automatically respond
            // with the expected response.
            KJ_IF_SOME(timerChannel, timer) {
              // This should count as a new IO event, hence we should call syncTime
              // otherwise the autoResponseTimestamp wouldn't be accurate.
              timerChannel.syncTime();
              hib.autoResponseTimestamp = timerChannel.now();
            } else {
              // Only a manager that adopted WebSockets from another process lacks a timer, until
              // its actor first runs. No JavaScript is running to observe the clock, so a coarse
              // reading reveals nothing it could time.
              hib.autoResponseTimestamp = kj::systemCoarseCalendarClock().now();
            }
            // We'll store the current timestamp in the HibernatableWebSocket to assure it gets
            // stored even if the WebSocket is currently hibernating. In that scenario, the timestamp
            // value will be loaded into the WebSocket during unhibernation.
            // Copy autoResponsePair->response into a coroutine-local kj::String before either
            // branch sends it. The hibernated branch's ws.send() borrows the underlying
            // ArrayPtr across the co_await per kj::WebSocket::send()'s documented contract,
            // and any concurrent JS call to state.setWebSocketAutoResponse() would reassign
            // or clear autoResponsePair->response, freeing the buffer while the write is
            // still in flight. The active branch's sendAutoResponse takes ownership of the
            // kj::String anyway, so hoisting the copy serves both cases with a single
            // allocation.
            auto responseCopy = kj::str(KJ_REQUIRE_NONNULL(autoResponsePair->response));
            KJ_SWITCH_ONEOF(hib.activeOrPackage) {
              KJ_CASE_ONEOF(apiWs, jsg::Ref<api::WebSocket>) {
                // Since we had a request set, we must have and response that's sent back using the
                // same websocket here. The sending of response is managed in web-socket to avoid
                // possible racing problems with regular websocket messages.
                hib.maybeWriteBarrier =
                    hib.writeCanceler
                        .wrap(apiWs->sendAutoResponse(kj::mv(responseCopy),
                            KJ_REQUIRE_NONNULL(hib.ws).downgrade(), hib.branchWriteBarrier()))
                        .fork();
                auto& promise = KJ_ASSERT_NONNULL(hib.maybeWriteBarrier);
                apiWs->setAutoResponseStatus(hib.autoResponseTimestamp, promise.addBranch());
                co_await promise;
              }
              KJ_CASE_ONEOF(package, api::WebSocket::HibernationPackage) {
                if (!package.closedOutgoingConnection) {
                  // We need to store the autoResponsePromise because we may instantiate an api::websocket
                  // If we do that, we have to provide it with the promise to avoid races. This can
                  // happen if we have a websocket hibernating, that unhibernates and sends a
                  // message while ws.send() for auto-response is also sending.
                  auto response = hib.branchWriteBarrier().then(
                      [weakWs = KJ_REQUIRE_NONNULL(hib.ws).downgrade(),
                          responseCopy = kj::mv(responseCopy)]() mutable {
                    return KJ_REQUIRE_NONNULL(
                        weakWs.tryGet(), "WebSocket destroyed before auto-response")
                        .send(responseCopy.asArray())
                        .attach(kj::mv(responseCopy));
                  });
                  hib.maybeWriteBarrier = hib.writeCanceler.wrap(kj::mv(response)).fork();
                  co_await KJ_ASSERT_NONNULL(hib.maybeWriteBarrier);
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
    auto eventWebSocketId = kj::str(websocketId);
    registerEventWebSocket(kj::str(websocketId), hib);
    KJ_DEFER(cancelEvent(eventWebSocketId));

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
    auto workerInterface = getWorkerForEvent(hib);
    co_await workerInterface->customEvent(
        kj::rc<api::HibernatableWebSocketCustomEvent>(hibernationEventType, kj::mv(params))
            .toOwn());
    if (isClose) {
      co_return;
    }
  }
}

kj::Promise<kj::Maybe<kj::WebSocket::Message>> LegacyHibernationManagerImpl::nextMessage(
    HibernatableWebSocket& hib) {
  while (true) {
    if (hib.exported) {
      co_return kj::none;
    }
    if (holdCount == 0) {
      if (!hib.pendingMessages.empty()) {
        auto message = kj::mv(hib.pendingMessages.front());
        hib.pendingMessages.pop_front();
        hib.wakeReceiver();
        co_return kj::mv(message);
      }
      KJ_IF_SOME(error, hib.receiveError) {
        kj::throwFatalException(error.clone());
      }
    }
    auto paf = kj::newPromiseAndFulfiller<void>();
    hib.deliveryWaiter = kj::mv(paf.fulfiller);
    co_await paf.promise;
  }
}

// Holds a manager's events for as long as it exists. Holds a reference to the manager, so that
// the hold ends wherever the manager is by then.
class LegacyHibernationManagerImpl::EventHold {
 public:
  explicit EventHold(kj::Rc<LegacyHibernationManagerImpl> manager): manager(kj::mv(manager)) {
    if (this->manager->holdCount++ == 0) {
      // Receivers take everything while events are held.
      for (auto& hib: this->manager->allWs) {
        hib->wakeReceiver();
      }
    }
  }
  ~EventHold() noexcept(false) {
    manager->releaseHold();
  }
  KJ_DISALLOW_COPY_AND_MOVE(EventHold);

 private:
  kj::Rc<LegacyHibernationManagerImpl> manager;
};

kj::Own<void> LegacyHibernationManagerImpl::holdEvents() {
  return kj::heap<EventHold>(addRefToThis());
}

void LegacyHibernationManagerImpl::releaseHold() {
  KJ_ASSERT(holdCount > 0);
  if (--holdCount == 0) {
    for (auto& hib: allWs) {
      hib->wakeDelivery();
    }
  }
}

void LegacyHibernationManagerImpl::attachMigrationLink(
    api::WebSocket& ws, kj::Own<WebSocketMigrationLink> link) {
  // The WebSocket was accepted moments ago, and acceptWebSocket() adds to the front.
  for (auto& hib: allWs) {
    KJ_IF_SOME(active, hib->activeOrPackage.tryGet<jsg::Ref<api::WebSocket>>()) {
      if (active.get() == &ws) {
        hib->migrationLink = kj::mv(link);
        return;
      }
    }
  }
}

namespace {

kj::WebSocket::Message cloneMessage(const kj::WebSocket::Message& message) {
  KJ_SWITCH_ONEOF(message) {
    KJ_CASE_ONEOF(text, kj::String) {
      return kj::str(text);
    }
    KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
      return kj::heapArray(data.asPtr());
    }
    KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
      return kj::WebSocket::Close{.code = close.code, .reason = kj::str(close.reason)};
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace

class LegacyHibernationManagerImpl::MigrationImpl final: public Migration {
 public:
  MigrationImpl(
      kj::Rc<LegacyHibernationManagerImpl> managerParam, kj::Array<HibernatableWebSocket*> sockets)
      : manager(kj::mv(managerParam)),
        sockets(kj::mv(sockets)),
        hold(kj::heap<EventHold>(manager.addRef())) {
    manager->migrating = true;
  }
  ~MigrationImpl() noexcept(false) {
    manager->migrating = false;
  }
  KJ_DISALLOW_COPY_AND_MOVE(MigrationImpl);

  // While events are held, no WebSocket's read loop can end, so none of `sockets` is removed until
  // finish() exports them.

  size_t size() override {
    return sockets.size();
  }

  kj::Maybe<WebSocketMigrationLink&> getLink(size_t index) override {
    KJ_REQUIRE(!finished, "the migration already finished");
    return sockets[index]->migrationLink.map(
        [](kj::Own<WebSocketMigrationLink>& link) -> WebSocketMigrationLink& { return *link; });
  }

  kj::Promise<void> whenReceived(size_t index, uint64_t count) override {
    KJ_REQUIRE(!finished, "the migration already finished");
    auto& hib = *sockets[index];
    if (hib.receivedCount >= count) {
      return kj::READY_NOW;
    }
    KJ_IF_SOME(error, hib.receiveError) {
      return error.clone();
    }
    auto paf = kj::newPromiseAndFulfiller<void>();
    hib.countWaiters.add(HibernatableWebSocket::CountWaiter{
      .count = count,
      .fulfiller = kj::mv(paf.fulfiller),
    });
    return kj::mv(paf.promise);
  }

  kj::Promise<void> whenSendsSettled() override {
    KJ_REQUIRE(!finished, "the migration already finished");
    auto barriers = kj::heapArrayBuilder<kj::Promise<void>>(sockets.size());
    for (auto hib: sockets) {
      barriers.add(hib->branchWriteBarrier());
    }
    return kj::joinPromisesFailFast(barriers.finish());
  }

  void requireUnchanged() {
    KJ_REQUIRE(!finished, "the migration already finished");
    KJ_REQUIRE(manager->allWs.size() == sockets.size(),
        "a WebSocket was accepted while its actor's WebSockets were moving");
    for (auto hib: sockets) {
      KJ_REQUIRE(hib->receiveError == kj::none, "a WebSocket ended while it was moving");
      KJ_REQUIRE(hib->activeOrPackage.is<api::WebSocket::HibernationPackage>(),
          "a WebSocket woke up while it was moving");
    }
  }

  kj::Array<MigratedWebSocket> exportWebSockets() override {
    requireUnchanged();
    auto result = kj::heapArrayBuilder<MigratedWebSocket>(sockets.size());
    for (auto hib: sockets) {
      auto& package = hib->activeOrPackage.get<api::WebSocket::HibernationPackage>();
      auto copy = [](const kj::Maybe<kj::String>& value) {
        return value.map([](const kj::String& s) { return kj::str(s); });
      };
      auto pending = kj::heapArrayBuilder<kj::WebSocket::Message>(hib->pendingMessages.size());
      for (auto& message: hib->pendingMessages) {
        pending.add(cloneMessage(message));
      }
      result.add(MigratedWebSocket{
        .url = copy(package.url),
        .protocol = copy(package.protocol),
        .extensions = copy(package.extensions),
        .serializedAttachment = package.serializedAttachment.map(
            [](const kj::Array<kj::byte>& bytes) { return kj::heapArray(bytes.asPtr()); }),
        .tags = hib->getTags(),
        .closedOutgoingConnection = package.closedOutgoingConnection,
        .allowHalfOpen = package.allowHalfOpen == api::AllowHalfOpen::YES,
        .autoResponseTimestamp = hib->autoResponseTimestamp,
        .pendingMessages = pending.finish(),
      });
    }
    return result.finish();
  }

  kj::Own<void> finish() override {
    requireUnchanged();
    finished = true;
    auto connections = kj::heapArrayBuilder<kj::Rc<kj::WebSocket>>(sockets.size());
    for (auto hib: sockets) {
      hib->pendingMessages.clear();
      // The caller keeps the connection open until its request has completed. Stop receiving
      // first, since the receiver borrows the connection.
      hib->receiverTask = kj::none;
      connections.add(KJ_ASSERT_NONNULL(kj::mv(hib->ws)));
      hib->ws = kj::none;

      // Ends the read loop, which removes the WebSocket without delivering anything.
      hib->exported = true;
      ++manager->exportedCount;
      hib->wakeDelivery();
    }
    return kj::heap(connections.finish());
  }

 private:
  kj::Rc<LegacyHibernationManagerImpl> manager;
  kj::Array<HibernatableWebSocket*> sockets;
  kj::Own<EventHold> hold;
  bool finished = false;
};

kj::Maybe<kj::Own<Worker::Actor::HibernationManager::Migration>> LegacyHibernationManagerImpl::
    tryBeginMigration() {
  if (migrating || allWs.empty()) {
    return kj::none;
  }
  auto sockets = kj::heapArrayBuilder<HibernatableWebSocket*>(allWs.size());
  for (auto& hib: allWs) {
    if (hib->receiveError != kj::none || hib->exported || hib->hasDispatchedClose ||
        !hib->activeOrPackage.is<api::WebSocket::HibernationPackage>()) {
      return kj::none;
    }
    sockets.add(hib.get());
  }
  return kj::Own<Migration>(kj::heap<MigrationImpl>(addRefToThis(), sockets.finish()));
}

Worker::Actor::HibernationManager::MigratedSettings LegacyHibernationManagerImpl::exportSettings() {
  return MigratedSettings{
    .autoResponseRequest = autoResponsePair->request.map([](auto& s) { return kj::str(s); }),
    .autoResponseResponse = autoResponsePair->response.map([](auto& s) { return kj::str(s); }),
    .eventTimeoutMs = eventTimeoutMs,
  };
}

void LegacyHibernationManagerImpl::importSettings(MigratedSettings settings) {
  autoResponsePair->request = kj::mv(settings.autoResponseRequest);
  autoResponsePair->response = kj::mv(settings.autoResponseResponse);
  eventTimeoutMs = settings.eventTimeoutMs;
}

void LegacyHibernationManagerImpl::adoptWebSocket(MigratedWebSocket socket,
    kj::Own<kj::WebSocket> ws,
    kj::Maybe<kj::Own<WebSocketMigrationLink>> migrationLink) {
  JSG_REQUIRE(allWs.size() < ACTIVE_CONNECTION_LIMIT, Error, "only ", ACTIVE_CONNECTION_LIMIT,
      " websockets can be accepted on a single Durable Object instance");

  auto hib = kj::heap<HibernatableWebSocket>(
      api::WebSocket::HibernationPackage{
        .url = kj::mv(socket.url),
        .protocol = kj::mv(socket.protocol),
        .extensions = kj::mv(socket.extensions),
        .serializedAttachment = kj::mv(socket.serializedAttachment),
        .maybeTags = kj::none,
        .closedOutgoingConnection = socket.closedOutgoingConnection,
        .allowHalfOpen = api::AllowHalfOpen(socket.allowHalfOpen),
      },
      socket.tags.size(), *this);
  HibernatableWebSocket& refToHibernatable = *hib;
  allWs.push_front(kj::mv(hib));
  refToHibernatable.node = allWs.begin();
  insertTags(refToHibernatable, socket.tags);

  refToHibernatable.autoResponseTimestamp = socket.autoResponseTimestamp;
  for (auto& message: socket.pendingMessages) {
    refToHibernatable.pendingMessages.push_back(kj::mv(message));
  }
  refToHibernatable.migrationLink = kj::mv(migrationLink);
  refToHibernatable.ws = kj::Rc<kj::WebSocket>(kj::mv(ws));
  startReadLoop(refToHibernatable);
}

};  // namespace workerd
