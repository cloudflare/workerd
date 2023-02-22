// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <workerd/api/web-socket.h>
#include <workerd/io/worker.h>
#include "v8-isolate.h"
#include <workerd/jsg/ser.h>
#include <kj/linked-list.h>

namespace workerd {

class HibernationManagerImpl final : public Worker::Actor::HibernationManager {
  // Implements the HibernationManager class.
public:
  HibernationManagerImpl(kj::Maybe<WorkerInterface&> worker)
      : workerInterface(kj::mv(worker)), onDisconnect(DisconnectHandler{}), readLoopTasks(onDisconnect) {}
  ~HibernationManagerImpl();

  void acceptWebSocket(jsg::Ref<api::WebSocket> ws, kj::ArrayPtr<kj::String> tags) override;
  // Tells the HibernationManager to create a new HibernatableWebSocket with the associated tags
  // and to initiate the `readLoop()` for this websocket.

  kj::Vector<jsg::Ref<api::WebSocket>> getWebSockets(jsg::Lock& js, kj::StringPtr tag) override;
  // Gets a collection of websockets associated with the given tag. Any hibernating websockets will
  // be woken up.

  kj::Array<kj::byte> serializeV8Value(v8::Local<v8::Value> value, v8::Isolate* isolate);
  // We need to serialize the `attachment` for each websocket when they hibernate.

  // TODO(now): Should we include a deseriailizeV8Value() in this class, or elsewhere?

  void hibernateWebSockets(jsg::Lock& js, v8::Isolate* isolate, IoContext& context) override;
  // Hibernates all the websockets held by the HibernationManager.
  // This involves:
  //    - Seriailizing the `attachment`
  //    - Dropping the api::WebSocket reference (`activeWebSocket`)

private:
  class HibernatableWebSocket;
  struct TagListItem {
    // Each HibernatableWebSocket can have multiple tags, so we want to store a reference
    // in our kj::List.
    kj::Maybe<HibernatableWebSocket&> hibWS;
    kj::ListLink<TagListItem> link;
    kj::StringPtr tag;
    kj::Maybe<kj::List<TagListItem, &TagListItem::link>&> list;
    // The List that refers to this TagListItem.
    // If `list` is null, we've already removed this item from the list.
  };

  class HibernatableWebSocket {
    // api::WebSockets cannot survive hibernation, but kj::WebSockets do. This class helps us
    // manage the transition of an api::WebSocket from its active state to a hibernated state
    // and vice versa.
    //
    // Some properties of the JS websocket object need to be retained throughout hibernation,
    // such as `attachment`, `url`, `extensions`, etc. These properties are only read/modified
    // when initiating, or waking from hibernation.
  public:
    HibernatableWebSocket(jsg::Ref<api::WebSocket> websocket,
                          kj::ArrayPtr<kj::String> tags,
                          size_t id, HibernationManagerImpl& manager)
        : tagItems(kj::heapArray<TagListItem>(tags.size())),
          activeWebSocket(kj::mv(websocket)),
          // Extract's the kj::Own<kj::WebSocket> from api::WebSocket so the HibernatableWebSocket
          // can own it. The api::WebSocket retains a reference to our ws.
          ws(KJ_REQUIRE_NONNULL(activeWebSocket)->acceptAsHibernatable()),
          id(id),
          manager(manager),
          url(KJ_REQUIRE_NONNULL(activeWebSocket)->getUrl()
              .map([](kj::StringPtr s) { return kj::str(s);})),
          protocol(KJ_REQUIRE_NONNULL(activeWebSocket)->getProtocol()
              .map([](kj::StringPtr s) { return kj::str(s);})),
          extensions(KJ_REQUIRE_NONNULL(activeWebSocket)->getExtensions()
              .map([](kj::StringPtr s) { return kj::str(s);})) {}

    ~HibernatableWebSocket() {
      // We expect this dtor to be called when we're removing a HibernatableWebSocket
      // from our `allWs` collection in the HibernationManager.

      // This removal is fast because we have direct access to each kj::List, as well as direct
      // access to each TagListItem we want to remove.
      for (auto& item: tagItems) {
        KJ_IF_MAYBE(list, item.list) {
          // The list reference is non-null, so we still have a valid reference to this
          // TagListItem in the list, which we will now remove.
          list->remove(item);
          if (list->empty()) {
            // Remove the bucket in tagToWs if the tag has no more websockets.
            manager.tagToWs.erase(kj::mv(item.tag));
          }
        }
        item.hibWS = nullptr;
        item.list = nullptr;
      }
    }

    kj::ListLink<HibernatableWebSocket> link;

    kj::Array<TagListItem> tagItems;
    // An array of all the items/nodes that refer to this HibernatableWebSocket.
    // Keeping track of these items allows us to quickly remove every reference from `tagToWs`
    // once the websocket disconnects -- rather than iterating through each relevant tag in the
    // hashmap and removing it from each kj::List.

    kj::Maybe<jsg::Ref<api::WebSocket>> activeWebSocket;
    // Null if we're hibernating, otherwise, a reference to the active api::WebSocket.
    kj::Own<kj::WebSocket> ws;
    size_t id;
    // Used for equality check.
    HibernationManagerImpl& manager;
    // TODO(now): We (currently) only use the HibernationManagerImpl reference to refer to `tagToWs`
    // when running the dtor for `HibernatableWebSocket`. This feels a bit excessive, I would rather
    // have the HibernationManager deal with its collections than have the HibernatableWebSocket
    // do so. Maybe come back to this at some point?
    kj::Maybe<kj::Node<HibernatableWebSocket>&> node;
    // Reference to the Node in `allWs` that allows us to do fast deletion on disconnect.

    // The following properties must be retained through hibernation.

    kj::Array<kj::byte> attachment;
    // Serialized attachment property of the api::WebSocket.
    // The `attachment` is read upon waking from hibernation, and written to upon hibernating.
    const kj::Maybe<kj::String> url;
    const kj::Maybe<kj::String> protocol;
    const kj::Maybe<kj::String> extensions;
    friend HibernationManagerImpl;

    bool operator==(HibernatableWebSocket& other) {
      return other.id == this->id;
    }
  };

  size_t countOfAcceptedWebSockets = 0;

private:
  void dropHibernatableWebSocket(HibernatableWebSocket& hib);
  // Removes a HibernatableWebSocket from the HibernationManager's various collections.

  inline void removeFromAllWs(HibernatableWebSocket& hib);
  // Removes the HibernatableWebSocket from `allWs`.

  kj::Promise<void> readLoop(HibernatableWebSocket& hib);
  // Like the api::WebSocket readLoop(), but we dispatch different types of events.

  struct TagCollection {
    // This struct is held by the `tagToWs` hashmap. The key is a StringPtr to tag, and the value
    // is this struct itself.
    kj::String tag;
    kj::Own<kj::List<TagListItem, &TagListItem::link>> list;

    TagCollection(kj::String tag, decltype(list) list): tag(kj::mv(tag)), list(kj::mv(list)) {}
    TagCollection(TagCollection&& other) = default;
  };

  kj::HashMap<kj::StringPtr, kj::Own<TagCollection>> tagToWs;
  // A hashmap of tags to HibernatableWebSockets associated with the tag.
  // We use a kj::List so we can quickly remove websockets that have disconnected.
  // Also note that we box the keys and values such that in the event of a hashmap resizing we don't
  // move the underlying data (thereby keeping any references intact).

  kj::LinkedList<HibernatableWebSocket> allWs;
  // We store all of our HibernatableWebSockets in a doubly linked-list.
  // Note that we actually heap allocate `HibernatableWebSocket` since the `data` of a Node
  // in the LinkedList must be boxed.

  kj::Maybe<WorkerInterface&> workerInterface;
  // TODO(now): Change this to `HibernationEventDispatcher`, WorkerInterface was an old idea.

  class DisconnectHandler: public kj::TaskSet::ErrorHandler {
  // TODO(now): rename because it's not necessarily on disconnect.
  public:
    void taskFailed(kj::Exception&& exception) override {
      // Called when a readLoop promise throws an exception.
    }
  };
  DisconnectHandler onDisconnect;
  kj::TaskSet readLoopTasks;
};
}; // namespace workerd
