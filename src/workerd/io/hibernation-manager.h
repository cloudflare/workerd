// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/actor-state.h>
#include <workerd/api/hibernatable-web-socket.h>
#include <workerd/api/web-socket.h>
#include <workerd/jsg/jsg.h>

#include <kj/debug.h>

#include <list>

namespace workerd {

// Implements the HibernationManager class.
class HibernationManagerImpl final: public Worker::Actor::HibernationManager {
 public:
  HibernationManagerImpl(kj::Own<Worker::Actor::Loopback> loopback, uint16_t hibernationEventType);
  ~HibernationManagerImpl() noexcept(false);

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
  kj::Maybe<jsg::Ref<api::WebSocketRequestResponsePair>> getWebSocketAutoResponse() override;
  void setTimerChannel(TimerChannel& timerChannel) override;

  kj::Own<HibernationManager> addRef() override;

  friend class api::HibernatableWebSocketEvent;

  // Sets/Unset the maximum time in milliseconds that an hibernatable websocket event can run for.
  // If the timeout is reached, event is canceled.
  void setEventTimeout(kj::Maybe<uint32_t> timeoutMs) override;

  // Gets the event timeout if set.
  kj::Maybe<uint32_t> getEventTimeout() override;

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
        HibernationManagerImpl& manager);
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

    HibernationManagerImpl& manager;
    // TODO(someday): We (currently) only use the HibernationManagerImpl reference to refer to
    // `tagToWs` when running the dtor for `HibernatableWebSocket`. This feels a bit excessive,
    // I would rather have the HibernationManager deal with its collections than have the
    // HibernatableWebSocket do so. Maybe come back to this at some point?

    // Reference to the Node in `allWs` that allows us to do fast deletion on disconnect.
    kj::Maybe<std::list<kj::Own<HibernatableWebSocket>>::iterator> node;

    // True once we have dispatched the close event.
    // This prevents us from dispatching it if we have already done so.
    bool hasDispatchedClose = false;

    // Stores the last received autoResponseRequest timestamp.
    kj::Maybe<kj::Date> autoResponseTimestamp;

    // Keeps track of the currently ongoing websocket auto-response send promise. This promise may
    // be moved to api::websocket if an hibernating websocket unhibernates.
    kj::Promise<void> autoResponsePromise = kj::READY_NOW;

    friend HibernationManagerImpl;
  };

 private:
  // Removes a HibernatableWebSocket from the HibernationManager's various collections.
  void dropHibernatableWebSocket(HibernatableWebSocket& hib);

  // Removes the HibernatableWebSocket from `allWs`.
  inline void removeFromAllWs(HibernatableWebSocket& hib);

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

  // Used to obtain the worker so we can dispatch Hibernatable websocket events.
  kj::Own<Worker::Actor::Loopback> loopback;

  // Passed to HibernatableWebSocket custom event as the typeId.
  uint16_t hibernationEventType;

  // A map of { ID -> HibernatableWebSocket } that allows the event handler that is currently
  // running to access the HibernatableWebSocket that it needs to execute.
  //
  // Dispatching events tends to result in races when events are received on different websockets
  // around the same time. Suppose there are two websockets that disconnect at the same time.
  // It is possible that both of them will be added to the map (i.e. their `receive()`
  // will throw) before the first event is dispatched and manages to obtain its associated websocket.
  kj::HashMap<kj::String, HibernatableWebSocket*> webSocketsForEventHandler;

  // The maximum number of Hibernatable WebSocket connections a single HibernationManagerImpl
  // instance can manage.
  const size_t ACTIVE_CONNECTION_LIMIT = 1024 * 32;

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
