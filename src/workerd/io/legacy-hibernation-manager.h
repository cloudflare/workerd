// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/actor-state.h>
#include <workerd/api/hibernatable-web-socket.h>
#include <workerd/api/web-socket.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>

#include <kj/exception.h>

#include <list>

namespace workerd {

struct LegacyHibernationManagerTestAccess;

// Implements the HibernationManager class.
class LegacyHibernationManagerImpl final: public Worker::Actor::HibernationManager {
 public:
  LegacyHibernationManagerImpl(
      kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType);
  ~LegacyHibernationManagerImpl() noexcept(false);

  // Tells the HibernationManager to create a new HibernatableWebSocket with the associated tags
  // and to initiate the `readLoop()` for this websocket. The `tags` array *must* contain only
  // unique elements.
  void acceptWebSocket(jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) override;

  // Gets a collection of websockets associated with the given tag. Any hibernating websockets will
  // be woken up. If no tag is provided, we return all accepted websockets.
  kj::Vector<jsg::Ref<api::WebSocket>> getWebSockets(
      jsg::Lock& js, kj::Maybe<kj::StringPtr> tag) override;

  // Hibernates all the websockets held by the HibernationManager.
  // This converts our activeOrPackage from an api::WebSocket to a HibernationPackage.
  void hibernateWebSockets(Worker::Lock& lock) override;

  void setWebSocketAutoResponse(
      kj::Maybe<kj::StringPtr> request, kj::Maybe<kj::StringPtr> response) override;
  kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse(
      jsg::Lock& js) override;
  void beginLoopbackHandoff() override;
  void cancelLoopbackHandoff() override;
  void setLoopback(kj::Own<Worker::Actor::Loopback> loopback) override;
  void setTimerChannel(TimerChannel& timerChannel) override;
  void setOwningActor(Worker::Actor& actor) override;
  kj::Maybe<Worker::Actor&> getOwningActor() override;
  kj::Maybe<const Worker::Actor::Id&> getOwningActorId() override;

  kj::Own<HibernationManager> addRef() override;

  friend class api::HibernatableWebSocketEvent;
  friend class api::HibernatableWebSocketCustomEvent;
  friend struct LegacyHibernationManagerTestAccess;

  // Sets/Unset the maximum time in milliseconds that an hibernatable websocket event can run for.
  // If the timeout is reached, event is canceled.
  void setEventTimeout(kj::Maybe<uint32_t> timeoutMs) override;

  // Gets the event timeout if set.
  kj::Maybe<uint32_t> getEventTimeout() override;

  // Returns the hibernation manager that registered `websocketId` for the hibernatable WebSocket
  // event currently being delivered, or kj::none when no manager on this event loop registered that
  // ID.
  //
  // The result is a bare reference on purpose. A dispatch path that held an owning reference would
  // keep the manager alive for as long as the event takes to settle, and the manager owns the task
  // delivering that event -- an event that never settles would then keep both alive forever.
  static kj::Maybe<Worker::Actor::HibernationManager&> findManagerForEvent(
      kj::StringPtr websocketId);

 private:
  class HibernatableWebSocket;

  kj::Promise<void> handleReadLoop(HibernatableWebSocket& refToHibernatable);

  // Each HibernatableWebSocket can have multiple tags, so we want to store a reference
  // in our kj::List.
  struct TagListItem {
    kj::Maybe<HibernatableWebSocket&> hibWS;
    kj::ListLink<TagListItem> link;
    kj::StringPtr tag;
    // The List that refers to this TagListItem.
    // If `list` is null, we've already removed this item from the list.
    kj::Maybe<kj::List<TagListItem, &TagListItem::link>&> list;
  };

  // api::WebSockets cannot survive hibernation, but kj::WebSockets do. This class helps us
  // manage the transition of an api::WebSocket from its active state to a hibernated state
  // and vice versa.
  //
  // Some properties of the JS websocket object need to be retained throughout hibernation,
  // such as `attachment`, `url`, `extensions`, etc. These properties are only read/modified
  // when initiating, or waking from hibernation.
  class HibernatableWebSocket {
   public:
    HibernatableWebSocket(jsg::Ref<api::WebSocket> websocket,
        kj::ArrayPtr<kj::String> tags,
        LegacyHibernationManagerImpl& manager);
    ~HibernatableWebSocket() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(HibernatableWebSocket);

    // Returns the tags associated with this HibernatableWebSocket.
    kj::Array<kj::StringPtr> getTags();

    // Returns the tags associated with this HibernatableWebSocket.
    // Note that this returns an array of Strings, unlike `getTags()`.
    // Copying the strings each time tags are requested would be expensive,
    // so we only do it when we're delivering a close/error event because
    // we will be destroying the HibernatableWebSocket object,
    // which the tags need to outlive.
    kj::Array<kj::String> cloneTags();

    // Returns a reference to the active websocket. If the websocket is currently hibernating,
    // we have to unhibernate it first. The process moves values from the HibernatableWebSocket
    // to the api::WebSocket.
    jsg::Ref<api::WebSocket> getActiveOrUnhibernate(jsg::Lock& js);

    kj::ListLink<HibernatableWebSocket> link;

    // An array of all the items/nodes that refer to this HibernatableWebSocket.
    // Keeping track of these items allows us to quickly remove every reference from `tagToWs`
    // once the websocket disconnects -- rather than iterating through each relevant tag in the
    // hashmap and removing it from each kj::List.
    kj::Array<TagListItem> tagItems;

    // If active, we have an api::WebSocket reference, otherwise, we're hibernating, so we retain
    // the websocket's properties in a HibernationPackage until it's time to wake up.
    kj::OneOf<jsg::Ref<api::WebSocket>, api::WebSocket::HibernationPackage> activeOrPackage;

    // This is an owned websocket that we extract from the api::WebSocket after accepting as
    // hibernatable. It becomes null once we dispatch a close or error event because we want its
    // lifetime to be managed by IoContext's DeleteQueue. This helps prevent a situation where the
    // HibernationManager drops the websocket before all queued messages have sent.
    kj::Maybe<kj::Own<kj::WebSocket>> ws;

    LegacyHibernationManagerImpl& manager;
    // TODO(someday): We (currently) only use the LegacyHibernationManagerImpl reference to refer to
    // `tagToWs` when running the dtor for `HibernatableWebSocket`. This feels a bit excessive,
    // I would rather have the HibernationManager deal with its collections than have the
    // HibernatableWebSocket do so. Maybe come back to this at some point?

    // Reference to the Node in `allWs` that allows us to do fast deletion on disconnect.
    kj::Maybe<std::list<kj::Own<HibernatableWebSocket>>::iterator> node;

    // True once we have dispatched the close event.
    // This prevents us from dispatching it if we have already done so.
    bool hasDispatchedClose = false;

    // Trace context captured at acceptWebSocket() time, restored when the DO is woken up
    // so that hibernation events are linked to the original trace.
    kj::Maybe<tracing::SpanContext> userSpanContext;

    // Stores the last received autoResponseRequest timestamp.
    kj::Maybe<kj::Date> autoResponseTimestamp;

    // Keeps track of the currently ongoing websocket auto-response send promise. This promise may
    // be moved to api::websocket if an hibernating websocket unhibernates.
    kj::Promise<void> autoResponsePromise = kj::READY_NOW;

    friend LegacyHibernationManagerImpl;
  };

  // Removes a HibernatableWebSocket from the HibernationManager's various collections.
  void dropHibernatableWebSocket(HibernatableWebSocket& hib);

  // Removes the HibernatableWebSocket from `allWs`.
  inline void removeFromAllWs(HibernatableWebSocket& hib);

  struct RegisteredEvent {
    // Neither pointer is owning. A registration lives only as long as the task delivering the
    // event, and ~LegacyHibernationManagerImpl drops any that remain, so a registered entry always
    // names a live manager and WebSocket.
    //
    // Pointers rather than references because this is a row of a kj::HashMap, and erasing a row
    // move-assigns the last row over it. That makes the type assignable, which a reference member
    // would not be; see the note on assignable types in KJ's style guide.
    //
    // The manager carries the event's identity: it names the actor that owns it.
    LegacyHibernationManagerImpl* manager;
    HibernatableWebSocket* webSocket;
  };

  // Maps each hibernatable WebSocket event ID currently being delivered to the manager and socket
  // that it belongs to. Delivery can take an RPC round trip that cannot carry a C++ reference, and
  // it can arrive at an actor whose current hibernation manager is not the one holding the socket
  // (a code-update wake installs a fresh actor), so the event ID is the only dependable route back
  // to the owner.
  //
  // The registry is per-event-loop: a manager, its native WebSockets and its JavaScript state all
  // belong to one event loop, and delivery always returns to that same loop. An event loop here does
  // not correspond to an OS thread, so neither thread locals nor thread identity would be sound.
  using EventRegistry = kj::HashMap<kj::String, RegisteredEvent>;
  static EventRegistry& getEventRegistry();

  // Removes and returns the WebSocket for the event currently being delivered. Fails if no manager
  // on this event loop holds that event ID.
  static HibernatableWebSocket& takeWebSocketForEvent(kj::StringPtr websocketId);

  // Records `hib` against an event ID, so that the handler that runs next can claim the WebSocket
  // and so that delivery can find this manager again. The registry owns its copy of the ID.
  void registerEventWebSocket(kj::String websocketId, HibernatableWebSocket& hib);

  // Removes an event registration. Safe to call after the receiver already claimed the event.
  void cancelEvent(kj::StringPtr websocketId);

  // Returns a worker to dispatch an event for `hib` on, carrying the trace context captured when
  // the WebSocket was accepted. Asks for pipeline re-resolution when no live actor owns this
  // manager, since the pipeline the loopback holds may name a version that is no longer current.
  kj::Own<WorkerInterface> getWorkerForEvent(HibernatableWebSocket& hib);

  // Handles the termination of the websocket. If termination was not clean, we might try to
  // dispatch a close event (if we haven't already), or an error event.
  // We will also remove the HibernatableWebSocket from the HibernationManager's collections.
  kj::Promise<void> handleSocketTermination(
      HibernatableWebSocket& hib, kj::Maybe<kj::Exception>& maybeError) KJ_WARN_UNUSED_RESULT;

  // Like the api::WebSocket readLoop(), but we dispatch different types of events.
  kj::Promise<void> readLoop(HibernatableWebSocket& hib);

  // This struct is held by the `tagToWs` hashmap. The key is a StringPtr to tag, and the value
  // is this struct itself.
  struct TagCollection {
    kj::String tag;
    kj::Own<kj::List<TagListItem, &TagListItem::link>> list;

    TagCollection(kj::String tag, decltype(list) list): tag(kj::mv(tag)), list(kj::mv(list)) {}
    TagCollection(TagCollection&& other) = default;
  };

  // This structure will hold the request and corresponding response for hibernatable websockets
  // auto-response feature. Although we store 2 kj::Maybe strings, if we don't have a request set
  // we can't have a response, and vice versa.
  // TODO(cleanup): Remove kj::Maybe from request and response strings.
  struct AutoRequestResponsePair {
    kj::Maybe<kj::String> request = kj::none;
    kj::Maybe<kj::String> response = kj::none;
  };

  // A hashmap of tags to HibernatableWebSockets associated with the tag.
  // We use a kj::List so we can quickly remove websockets that have disconnected.
  // Also note that we box the keys and values such that in the event of a hashmap resizing we don't
  // move the underlying data (thereby keeping any references intact).
  kj::HashMap<kj::StringPtr, kj::Own<TagCollection>> tagToWs;

  // We store all of our HibernatableWebSockets in a doubly linked-list.
  std::list<kj::Own<HibernatableWebSocket>> allWs;

  struct LoopbackWaiter {
    IoChannelFactory::SubrequestMetadata metadata;
    kj::Own<kj::PromiseFulfiller<kj::Own<WorkerInterface>>> fulfiller;
  };

  // The loopback events are delivered through. Null only while this manager is moving between
  // actor generations. Events that arrive in that window wait until the replacement actor supplies
  // its loopback.
  kj::Maybe<kj::Own<Worker::Actor::Loopback>> loopback;

  // The outgoing generation's loopback, parked here by beginLoopbackHandoff() so that
  // cancelLoopbackHandoff() can put it back if no replacement arrives. Set only for the duration of
  // a handoff, and so non-null exactly when `loopback` is null.
  kj::Maybe<kj::Own<Worker::Actor::Loopback>> handoffLoopback;

  // Events waiting out a handoff, served in order once a loopback is available again.
  kj::Vector<LoopbackWaiter> loopbackWaiters;

  // The actor that owns this manager, invalid once that actor is destroyed. Not inferrable from
  // `loopback`, which outlives the actor that supplied it and which cancelLoopbackHandoff()
  // restores while no actor is attached.
  kj::Maybe<kj::Own<Worker::Actor::WeakRef>> owningActor;

  // The ID of the actor named by `owningActor`, held as a copy so that it outlives that actor. A
  // code update destroys the owner before its replacement adopts the manager, so by the time
  // anything checks who may adopt it, this is the only identity still available.
  kj::Maybe<Worker::Actor::Id> owningActorId;

  // Passed to HibernatableWebSocket custom event as the typeId.
  uint16_t hibernationEventType;

  // How many of this manager's events are currently registered in the event loop's registry. Lets
  // the destructor skip the registry entirely when nothing is registered, which is the normal case
  // and the only case that can be reached without a current event loop.
  size_t registeredEventCount = 0;

  // The maximum number of Hibernatable WebSocket connections a single LegacyHibernationManagerImpl
  // instance can manage.
  const size_t ACTIVE_CONNECTION_LIMIT = 1024 * 32;

  // How long an event waits for a replacement loopback during a code-update handoff before giving
  // up. A handoff ends when the replacement actor supplies its loopback or when whoever began it
  // calls cancelLoopbackHandoff(), so this bound is only reached if some path does neither. It is
  // deliberately far longer than a handoff needs: it exists so that such a bug costs one failed
  // event rather than a WebSocket and read loop parked forever with nothing to say why.
  static constexpr kj::Duration LOOPBACK_HANDOFF_TIMEOUT = 30 * kj::SECONDS;

  class DisconnectHandler: public kj::TaskSet::ErrorHandler {
   public:
    // We don't need to do anything here; we already handle disconnects in the callee of readLoop().
    void taskFailed(kj::Exception&& exception) override {};
  };
  DisconnectHandler onDisconnect;
  kj::TaskSet readLoopTasks;
  kj::Own<AutoRequestResponsePair> autoResponsePair = kj::heap<AutoRequestResponsePair>();
  kj::Maybe<TimerChannel&> timer;
  kj::Maybe<uint32_t> eventTimeoutMs;
};
};  // namespace workerd
