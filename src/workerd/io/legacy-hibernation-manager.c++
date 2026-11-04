// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "legacy-hibernation-manager.h"

#include "io-channels.h"
#include "io-context.h"

#include <workerd/util/uuid.h>

#include <kj/common.h>
#include <kj/sticky-flag.h>

namespace workerd {

LegacyHibernationManagerImpl::EventRegistry& LegacyHibernationManagerImpl::getEventRegistry() {
  static const kj::EventLoopLocal<EventRegistry> registry;
  return *registry;
}

LegacyHibernationManagerImpl::HibernatableWebSocket::HibernatableWebSocket(
    kj::OneOf<jsg::Ref<api::WebSocket>, api::WebSocket::HibernationPackage> activeOrPackage,
    kj::ArrayPtr<kj::String> tags,
    kj::Rc<HibernatableTransport> transport,
    LegacyHibernationManagerImpl& manager)
    : tagItems(kj::heapArray<TagListItem>(tags.size())),
      activeOrPackage(kj::mv(activeOrPackage)),
      // The `ws` starts off empty because we need to set up our tagging infrastructure before
      // calling api::WebSocket::acceptAsHibernatable(). We will share ownership of the
      // kj::WebSocket before starting the readLoop. An adopted socket gets its `ws` from the
      // detached one.
      ws(kj::none),
      manager(manager),
      transport(kj::mv(transport)) {}

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

  // A detachment watches the transport only while this record holds it, and the transport can
  // outlive the record, as it does when the socket is taken out for a handoff.
  transport->setOnAttach(kj::none);
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

LegacyHibernationManagerImpl::PendingReceive::PendingReceive(
    kj::Promise<kj::WebSocket::Message> receive)
    : received(
          receive.then([this](kj::WebSocket::Message value) { message = kj::mv(value); }).fork()) {}

kj::Promise<void> LegacyHibernationManagerImpl::PendingReceive::whenReceived() {
  return received.addBranch();
}

kj::WebSocket::Message LegacyHibernationManagerImpl::PendingReceive::takeMessage() {
  return kj::mv(KJ_ASSERT_NONNULL(message));
}

kj::Promise<kj::WebSocket::Message> LegacyHibernationManagerImpl::PendingReceive::release(
    kj::Own<PendingReceive> self) {
  co_await self->whenReceived();
  co_return self->takeMessage();
}

kj::Promise<kj::WebSocket::Message> LegacyHibernationManagerImpl::HibernatableWebSocket::receive() {
  if (pendingReceive == kj::none) {
    auto& socket = *KJ_REQUIRE_NONNULL(ws);
    if (!transport->mayHaveHandle()) {
      // Only a handoff stops the read loop of a socket that lives on, and this one can't be
      // handed off.
      return socket.receive();
    }
    pendingReceive = kj::heap<PendingReceive>(socket.receive());
  }
  return receivePending();
}

kj::Promise<kj::WebSocket::Message> LegacyHibernationManagerImpl::HibernatableWebSocket::
    receivePending() {
  co_await KJ_ASSERT_NONNULL(pendingReceive)->whenReceived();
  auto message = KJ_ASSERT_NONNULL(pendingReceive)->takeMessage();
  pendingReceive = kj::none;
  co_return message;
}

kj::Promise<void> LegacyHibernationManagerImpl::HibernatableWebSocket::branchWriteBarrier() {
  KJ_IF_SOME(promise, maybeWriteBarrier) {
    return promise.addBranch();
  }
  return kj::READY_NOW;
}

jsg::Ref<api::WebSocket> LegacyHibernationManagerImpl::HibernatableWebSocket::
    getActiveOrUnhibernate(jsg::Lock& js) {
  KJ_IF_SOME(package, activeOrPackage.tryGet<api::WebSocket::HibernationPackage>()) {
    manager.abandonDetachment(WebSocketHandoffBlocker::WEBSOCKET_AWAKE);

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
    : loopback(kj::mv(loopback)),
      hibernationEventType(hibernationEventType),
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

kj::Own<Worker::Actor::HibernationManager> LegacyHibernationManagerImpl::addRef() {
  return kj::addRef(*this);
}

void LegacyHibernationManagerImpl::acceptWebSocket(
    jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) {
  // First, we create the HibernatableWebSocket and add it to the collection where it'll stay
  // until it's destroyed.

  JSG_REQUIRE(allWs.size() < ACTIVE_CONNECTION_LIMIT, Error, "only ", ACTIVE_CONNECTION_LIMIT,
      " websockets can be accepted on a single Durable Object instance");
  abandonDetachment(WebSocketHandoffBlocker::WEBSOCKET_AWAKE);

  auto hib =
      kj::heap<HibernatableWebSocket>(kj::mv(ws), tags, kj::rc<HibernatableTransport>(), *this);

  // TODO(mar): Improve accept span context capturing — route snapshotted user span context
  // to serialization point instead of capturing only the invocation root span here.
  auto invCtx = IoContext::current().getInvocationSpanContext();
  hib->userSpanContext = tracing::SpanContext(invCtx.getTraceId(), invCtx.getSpanId());

  auto& refToHibernatable = insertWebSocket(kj::mv(hib), tags);

  // Before starting the readLoop, share ownership of the kj::WebSocket with the
  // HibernatableWebSocket and accept the api::WebSocket as hibernatable.
  refToHibernatable.ws =
      refToHibernatable.activeOrPackage.get<jsg::Ref<api::WebSocket>>()->acceptAsHibernatable(
          refToHibernatable.getTags(), refToHibernatable.transport.downgrade());

  // Finally, we initiate the readloop for this HibernatableWebSocket and
  // give the task to the HibernationManager so it lives long.
  startReadLoop(refToHibernatable);
}

LegacyHibernationManagerImpl::HibernatableWebSocket& LegacyHibernationManagerImpl::insertWebSocket(
    kj::Own<HibernatableWebSocket> hib, kj::ArrayPtr<kj::String> tags) {
  HibernatableWebSocket& refToHibernatable = *hib.get();
  KJ_ASSERT(refToHibernatable.tagItems.size() == tags.size());

  allWs.push_front(kj::mv(hib));
  refToHibernatable.node = allWs.begin();

  // If the `tags` array is empty (i.e. user did not provide a tag), we skip the population of the
  // `tagToWs` HashMap below.

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
      return decltype(tagToWs)::Entry{kj::str(item->tag), kj::mv(item)};
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

  return refToHibernatable;
}

void LegacyHibernationManagerImpl::startReadLoop(HibernatableWebSocket& hib) {
  readLoopTasks.add(hib.readLoopCanceler.wrap(handleReadLoop(hib)).catch_([](kj::Exception&& e) {
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

uint64_t LegacyHibernationManagerImpl::getWebSocketCount() const {
  return allWs.size();
}

void LegacyHibernationManagerImpl::setWebSocketAutoResponse(
    kj::Maybe<kj::StringPtr> request, kj::Maybe<kj::StringPtr> response) {
  abandonDetachment(WebSocketHandoffBlocker::WEBSOCKETS_CHANGED);
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
  // Events no longer wait, so a message received from now on could reach the actor here.
  abandonDetachment(WebSocketHandoffBlocker::LOOPBACK_ARRIVED);

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
  LoopbackHandoff(kj::Own<LegacyHibernationManagerImpl> manager, uint64_t generation)
      : manager(kj::mv(manager)),
        generation(generation) {}
  ~LoopbackHandoff() noexcept(false) {
    manager->cancelLoopbackHandoff(generation);
  }
  KJ_DISALLOW_COPY_AND_MOVE(LoopbackHandoff);

 private:
  kj::Own<LegacyHibernationManagerImpl> manager;
  uint64_t generation;
};

kj::Own<void> LegacyHibernationManagerImpl::beginLoopbackHandoff() {
  KJ_REQUIRE(loopbackHandoffGeneration == 0, "a loopback handoff is already in progress");
  loopbackHandoffGeneration = nextLoopbackHandoffGeneration++;
  handoffLoopback = kj::mv(loopback);
  return kj::heap<LoopbackHandoff>(kj::addRef(*this), loopbackHandoffGeneration);
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
  abandonDetachment(WebSocketHandoffBlocker::WEBSOCKETS_CHANGED);
  eventTimeoutMs = kj::mv(timeoutMs);
}

kj::Maybe<uint32_t> LegacyHibernationManagerImpl::getEventTimeout() {
  return eventTimeoutMs;
}

kj::Maybe<Worker::Actor::HibernationManager&> LegacyHibernationManagerImpl::findManagerForEvent(
    kj::StringPtr websocketId) {
  KJ_IF_SOME(registered, getEventRegistry().find(websocketId)) {
    return *registered.manager;
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
  abandonDetachment(WebSocketHandoffBlocker::WEBSOCKETS_CHANGED);

  // The read loop calling this destroys `hib`, so `hib` must not cancel it.
  hib.readLoopCanceler.release();
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

  // The event can run only in the actor this manager belongs to.
  abandonDetachment(WebSocketHandoffBlocker::EVENT_IN_PROGRESS);

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
    kj::WebSocket::Message message = co_await hib.receive();
    // Note that errors are handled by the callee of `readLoop`, since we throw from `receive()`.
    countConsumed(hib);

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
            TimerChannel& timerChannel = KJ_REQUIRE_NONNULL(timer);
            // This should count as a new IO event, hence we should call syncTime
            // otherwise the autoResponseTimestamp wouldn't be accurate.
            timerChannel.syncTime();
            // We should have set the timerChannel previously in the hibernation manager.
            // If we haven't, we aren't able to get the current time.
            hib.autoResponseTimestamp = timerChannel.now();
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
    hib.deliveringEvent = true;
    KJ_DEFER(hib.deliveringEvent = false);
    auto workerInterface = getWorkerForEvent(hib);
    co_await workerInterface->customEvent(
        kj::rc<api::HibernatableWebSocketCustomEvent>(hibernationEventType, kj::mv(params))
            .toOwn());
    if (isClose) {
      co_return;
    }
  }
}

// =======================================================================================
// Handing the WebSockets to another manager

// A detachWebSocketsForHandoff() in progress. Readies the sockets, then holds them ready until
// take() takes them or the detachment is dropped.
class LegacyHibernationManagerImpl::Detachment final: public HibernatableWebSocketDetachment {
 public:
  explicit Detachment(LegacyHibernationManagerImpl& owner);
  ~Detachment() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(Detachment);

  // Pauses every transport, then waits until each socket has received what its transport sent,
  // and for any auto-response in progress.
  kj::Promise<void> ready();

  DetachedHibernatableWebSockets take() override;

  // Makes the detachment fail with what `blocker` causes, and resumes the transports. Only the
  // first call has an effect.
  void abandon(WebSocketHandoffBlocker blocker);

  // Counts a message that `hib` received toward the wait for every socket to catch up.
  void countConsumed(HibernatableWebSocket& hib);

 private:
  kj::Own<LegacyHibernationManagerImpl> manager;

  // Rejected, with `failure` set to the reason, as soon as the detachment can't succeed.
  kj::StickyFlag abandoned;
  kj::Maybe<kj::Exception> failure;

  // Signaled once every socket has received every message its transport sent before pausing.
  kj::StickyFlag caughtUp;

  // How many sockets haven't, counted once every transport has paused.
  kj::Maybe<size_t> laggingSockets;

  // True once take() has taken the sockets, after which the manager is free for another
  // detachment.
  bool taken = false;

  // Waits for `promise`, but fails if the detachment is abandoned first, or meanwhile.
  kj::Promise<void> unlessAbandoned(kj::Promise<void> promise);

  // Drops the pauses of the sockets in the manager, which resumes their transports.
  void releasePauses();
};

LegacyHibernationManagerImpl::Detachment::Detachment(LegacyHibernationManagerImpl& owner)
    : manager(kj::addRef(owner)) {
  owner.detachment = *this;
  for (auto& hib: owner.allWs) {
    // Another transport makes the pause stale, and restarts the count of received messages.
    hib->transport->setOnAttach(kj::Function<void()>(
        [this]() { abandon(WebSocketHandoffBlocker::WEBSOCKETS_CHANGED); }));
  }
}

LegacyHibernationManagerImpl::Detachment::~Detachment() noexcept(false) {
  if (!taken) {
    manager->detachment = kj::none;
    for (auto& hib: manager->allWs) {
      hib->transport->setOnAttach(kj::none);
    }
    releasePauses();
  }
}

void LegacyHibernationManagerImpl::Detachment::releasePauses() {
  for (auto& hib: manager->allWs) {
    hib->handoffCanceler.cancel("hibernatable WebSocket handoff ended");
    hib->handoffPause = kj::none;
  }
}

void LegacyHibernationManagerImpl::Detachment::abandon(WebSocketHandoffBlocker blocker) {
  if (failure != kj::none) {
    return;
  }
  auto exception = makeWebSocketHandoffException(blocker);
  failure = exception.clone();
  abandoned.reject(kj::mv(exception));
  releasePauses();
}

kj::Promise<void> LegacyHibernationManagerImpl::Detachment::unlessAbandoned(
    kj::Promise<void> promise) {
  kj::Maybe<kj::Exception> error;
  try {
    co_await promise.exclusiveJoin(abandoned.whenSignaled());
  } catch (...) {
    error = kj::getCaughtExceptionAsKj();
  }
  // Prefer the reason for abandoning to a failure it caused, such as a cancelled pause.
  KJ_IF_SOME(exception, failure) {
    kj::throwFatalException(exception.clone());
  }
  KJ_IF_SOME(exception, error) {
    kj::throwFatalException(kj::mv(exception));
  }
}

kj::Promise<void> LegacyHibernationManagerImpl::Detachment::ready() {
  auto& allWs = manager->allWs;
  auto pauses = kj::heapArrayBuilder<kj::Promise<void>>(allWs.size());
  for (auto& hib: allWs) {
    pauses.add(hib->handoffCanceler.wrap(manager->pauseTransport(*hib)));
  }
  co_await unlessAbandoned(kj::joinPromisesFailFast(pauses.finish()));

  // Wait until each socket has received every message its transport sent before pausing. One that
  // matches the auto-response is answered meanwhile. Any other needs the actor, so its event waits
  // for a loopback, which abandons the detachment.
  size_t lagging = 0;
  for (auto& hib: allWs) {
    auto sent = KJ_ASSERT_NONNULL(hib->handoffPause).sentCount;
    auto consumed = hib->transport->getConsumedCount();
    if (consumed > sent) {
      abandon(WebSocketHandoffBlocker::TRANSPORT_MISCOUNTED);
      kj::throwFatalException(KJ_ASSERT_NONNULL(failure).clone());
    }
    if (consumed < sent) {
      ++lagging;
    }
  }
  if (lagging > 0) {
    laggingSockets = lagging;
    co_await unlessAbandoned(caughtUp.whenSignaled());
  }

  // Wait for auto-responses still being written. Nothing can start another, because no socket
  // will receive another message.
  auto barriers = kj::heapArrayBuilder<kj::Promise<void>>(allWs.size());
  for (auto& hib: allWs) {
    barriers.add(hib->branchWriteBarrier());
  }
  co_await unlessAbandoned(kj::joinPromisesFailFast(barriers.finish()));
}

void LegacyHibernationManagerImpl::Detachment::countConsumed(HibernatableWebSocket& hib) {
  KJ_IF_SOME(pause, hib.handoffPause) {
    auto consumed = hib.transport->getConsumedCount();
    if (consumed > pause.sentCount) {
      abandon(WebSocketHandoffBlocker::TRANSPORT_MISCOUNTED);
    } else if (consumed == pause.sentCount) {
      KJ_IF_SOME(lagging, laggingSockets) {
        if (--lagging == 0) {
          caughtUp.signal();
        }
      }
    }
  }
}

DetachedHibernatableWebSockets LegacyHibernationManagerImpl::Detachment::take() {
  KJ_REQUIRE(!taken, "hibernatable WebSockets were already taken");
  KJ_IF_SOME(exception, failure) {
    kj::throwFatalException(exception.clone());
  }
  auto& owner = *manager;
  // Anything that could break these abandons the detachment.
  KJ_ASSERT(owner.loopback == kj::none && owner.checkDetachable() == kj::none);

  DetachedHibernatableWebSockets result;
  KJ_IF_SOME(request, owner.autoResponsePair->request) {
    result.autoResponse = DetachedHibernatableWebSockets::AutoResponse{
      .request = kj::str(request),
      .response = kj::str(KJ_ASSERT_NONNULL(owner.autoResponsePair->response)),
    };
  }
  result.eventTimeoutMs = owner.eventTimeoutMs;

  // In the order the sockets were accepted, which adoption preserves.
  auto sockets = kj::heapArrayBuilder<DetachedHibernatableWebSockets::Socket>(owner.allWs.size());
  for (auto it = owner.allWs.rbegin(); it != owner.allWs.rend(); ++it) {
    auto& hib = **it;
    KJ_ASSERT(hib.transport->getConsumedCount() == KJ_ASSERT_NONNULL(hib.handoffPause).sentCount);

    // The read loop is waiting for a message, or has just answered one with the auto-response.
    // Either way, the receive it may be waiting for runs on in `pendingReceive`.
    hib.readLoopCanceler.cancel("hibernatable WebSocket detached for handoff");
    sockets.add(DetachedHibernatableWebSockets::Socket{
      .ws = kj::mv(KJ_ASSERT_NONNULL(hib.ws)),
      .pendingReceive = hib.pendingReceive.map([](kj::Own<PendingReceive>& pending) {
      return PendingReceive::release(kj::mv(pending));
    }),
      .package = kj::mv(hib.activeOrPackage.get<api::WebSocket::HibernationPackage>()),
      .tags = hib.getTags(),
      .autoResponseTimestamp = hib.autoResponseTimestamp,
      .userSpanContext = kj::mv(hib.userSpanContext),
      .transport = hib.transport.addRef(),
      .pause = kj::mv(hib.handoffPause),
    });
  }
  result.sockets = sockets.finish();

  taken = true;
  owner.detachment = kj::none;
  // Destroying the records also removes them from the tag lists, and stops their transports from
  // reporting to this detachment.
  owner.allWs.clear();
  return result;
}

void LegacyHibernationManagerImpl::abandonDetachment(WebSocketHandoffBlocker blocker) {
  KJ_IF_SOME(state, detachment) {
    state.abandon(blocker);
  }
}

void LegacyHibernationManagerImpl::countConsumed(HibernatableWebSocket& hib) {
  hib.transport->countConsumed();
  KJ_IF_SOME(state, detachment) {
    state.countConsumed(hib);
  }
}

kj::Promise<void> LegacyHibernationManagerImpl::pauseTransport(HibernatableWebSocket& hib) {
  hib.handoffPause = co_await KJ_ASSERT_NONNULL(hib.transport->getHandle()).pause();
}

kj::Maybe<WebSocketHandoffBlocker> LegacyHibernationManagerImpl::checkDetachable() {
  // A registration names its socket record, which detachment destroys.
  if (registeredEventCount > 0) {
    return WebSocketHandoffBlocker::EVENT_IN_PROGRESS;
  }
  for (auto& hib: allWs) {
    if (hib->deliveringEvent) {
      return WebSocketHandoffBlocker::EVENT_IN_PROGRESS;
    }
    KJ_IF_SOME(package, hib->activeOrPackage.tryGet<api::WebSocket::HibernationPackage>()) {
      // Moving the socket ends its old transport with a Close, which the socket can't send twice.
      if (package.closedOutgoingConnection) {
        return WebSocketHandoffBlocker::WEBSOCKET_CLOSED;
      }
    } else {
      return WebSocketHandoffBlocker::WEBSOCKET_AWAKE;
    }
    if (hib->transport->getHandle() == kj::none) {
      return WebSocketHandoffBlocker::NO_TRANSPORT;
    }
  }
  return kj::none;
}

kj::Maybe<WebSocketHandoffBlocker> LegacyHibernationManagerImpl::checkWebSocketHandoff(
    size_t maxStateBytes) {
  KJ_IF_SOME(blocker, checkDetachable()) {
    return blocker;
  }

  auto sizeOf = [](const auto& maybe) -> size_t {
    KJ_IF_SOME(value, maybe) {
      return value.size();
    }
    return 0;
  };
  size_t stateBytes = sizeOf(autoResponsePair->request) + sizeOf(autoResponsePair->response);
  for (auto& hib: allWs) {
    auto& package = hib->activeOrPackage.get<api::WebSocket::HibernationPackage>();
    stateBytes += WEBSOCKET_HANDOFF_BYTES_PER_SOCKET + sizeOf(package.url) +
        sizeOf(package.protocol) + sizeOf(package.extensions) +
        sizeOf(package.serializedAttachment);
    for (auto& item: hib->tagItems) {
      stateBytes += WEBSOCKET_HANDOFF_BYTES_PER_TAG + item.tag.size();
    }
  }
  if (stateBytes > maxStateBytes) {
    return WebSocketHandoffBlocker::STATE_TOO_LARGE;
  }
  return kj::none;
}

kj::Promise<kj::Own<HibernatableWebSocketDetachment>> LegacyHibernationManagerImpl::
    detachWebSocketsForHandoff() {
  KJ_REQUIRE(detachment == kj::none, "hibernatable WebSockets are already being detached");
  KJ_REQUIRE(loopback == kj::none,
      "detaching hibernatable WebSockets requires events to wait for a loopback");
  KJ_IF_SOME(blocker, checkDetachable()) {
    kj::throwFatalException(makeWebSocketHandoffException(blocker));
  }

  auto state = kj::heap<Detachment>(*this);
  co_await state->ready();
  co_return kj::mv(state);
}

void LegacyHibernationManagerImpl::adoptWebSockets(DetachedHibernatableWebSockets detached) {
  KJ_REQUIRE(detachment == kj::none, "can't adopt hibernatable WebSockets while detaching them");
  KJ_REQUIRE(allWs.size() + detached.sockets.size() <= ACTIVE_CONNECTION_LIMIT,
      "too many hibernatable WebSockets to adopt", allWs.size(), detached.sockets.size());
  // The read loop records the time it answers with the auto-response.
  KJ_REQUIRE(detached.autoResponse == kj::none || timer != kj::none,
      "adopting hibernatable WebSockets with an auto-response requires a timer channel");

  KJ_IF_SOME(autoResponse, detached.autoResponse) {
    setWebSocketAutoResponse(
        kj::StringPtr(autoResponse.request), kj::StringPtr(autoResponse.response));
  } else {
    setWebSocketAutoResponse(kj::none, kj::none);
  }
  setEventTimeout(detached.eventTimeoutMs);

  for (auto& socket: detached.sockets) {
    auto hib = kj::heap<HibernatableWebSocket>(
        kj::mv(socket.package), socket.tags, kj::mv(socket.transport), *this);
    hib->ws = kj::mv(socket.ws);
    hib->pendingReceive =
        socket.pendingReceive.map([](kj::Promise<kj::WebSocket::Message>& receive) {
      return kj::heap<PendingReceive>(kj::mv(receive));
    });
    hib->autoResponseTimestamp = socket.autoResponseTimestamp;
    hib->userSpanContext = kj::mv(socket.userSpanContext);
    startReadLoop(insertWebSocket(kj::mv(hib), socket.tags));
  }

  // Returning drops `detached`, and with it any pauses, once every read loop is running.
}

};  // namespace workerd
