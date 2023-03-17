// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-context.h"
#include <workerd/io/hibernation-manager.h>

namespace workerd {

HibernationManagerImpl::~HibernationManagerImpl() {
  // Note that the HibernatableWebSocket destructor handles removing any references to itself in
  // `tagToWs`, and even removes the hashmap entry if there are no more entries in the bucket.
  allWs.clear();
  KJ_ASSERT(tagToWs.size() == 0, "tagToWs hashmap wasn't cleared.");
  KJ_ASSERT(allWs.size() == 0 && allWs.empty(), "allWs collection wasn't cleared.");
}

void HibernationManagerImpl::acceptWebSocket(jsg::Ref<api::WebSocket> ws,
    kj::ArrayPtr<kj::String> tags) {
  // First, we create the HibernatableWebSocket and add it to the collection where it'll stay
  // until it's destroyed.
  auto hib = kj::heap<HibernatableWebSocket>(kj::mv(ws), tags, countOfAcceptedWebSockets++, *this);
  HibernatableWebSocket& refToHibernatable = *hib.get();
  allWs.push_front(kj::mv(hib));
  refToHibernatable.node = allWs.begin();

  // Next, we add the HibernatableWebSocket to each bucket in `tagToWs` corresponding to its tags.
  // The process looks like:
  //  1. Create the entry if it doesn't exist
  //  2. Fill the TagListItem in the HibernatableWebSocket's tagItems array
  //  3. Initiate the readLoop.
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

  // Now that we've populated `tagToWs`, start the readloop for this HibernatableWebSocket.
  kj::Promise<kj::Maybe<kj::Exception>> readLoopPromise = kj::evalNow([&] {
    return readLoop(refToHibernatable);
  }).then([]() -> kj::Maybe<kj::Exception> { return nullptr; },
          [](kj::Exception&& e) -> kj::Maybe<kj::Exception> { return kj::mv(e); });

  // Give the task to the HibernationManager so it lives long.
  readLoopTasks.add(readLoopPromise.then(
      [&refToHibernatable, this](kj::Maybe<kj::Exception>&& e) {
    // We've disconnected cleanly, let's remove from our collections.
    //
    // TODO(now): We need to take the code in the api::WebSocket readLoop (that sets it to released)
    //            and do the same stuff here. Technically, if the object is loaded in JS it
    //            can still be used even after we drop our ref in the HibernationManager,
    //            so we need to set it to Released, and do whatever else.
    //
    //    Since we don't have access to the relevant properties of api::WebSocket, we should
    //    instead turn it into a public method.
    return dropHibernatableWebSocket(refToHibernatable);
  }, [](kj::Exception&& e) {
    // TODO(now): We've disconnected in an unclean way?
    return kj::mv(e);
  }));
}

kj::Vector<jsg::Ref<api::WebSocket>> HibernationManagerImpl::getWebSockets(
    jsg::Lock& js,
    kj::StringPtr tag) {
  kj::Vector<jsg::Ref<api::WebSocket>> matches;
  KJ_IF_MAYBE(item, tagToWs.find(tag)) {
    auto& list = *((*item)->list);
    for (auto& entry: list) {
      auto& hibWS = KJ_REQUIRE_NONNULL(entry.hibWS);
      // If the websocket is hibernating, we have to create an api::WebSocket
      // and add it to the HibernatableWebSocket.
      if (hibWS.activeWebSocket == nullptr) {
        // Note that we read the `attachment`, and in the process of doing so we will
        // deserialize the value so it can be accessed and modified in JS code.
        hibWS.activeWebSocket.emplace(
            api::WebSocket::unhibernate(js, *hibWS.ws, hibWS.attachment, hibWS.url,
                                        hibWS.protocol, hibWS.extensions, hibWS.autoResponseTimestamp));
      }
      // Now that we know the websocket is "awake", we simply add it to the vector.
      KJ_IF_MAYBE(awake, hibWS.activeWebSocket) {
        matches.add(awake->addRef());
      }
    }
  };
  return kj::mv(matches);
}

void HibernationManagerImpl::setWebSocketAutoResponse(kj::String request,
                                                      kj::String response) {
  autoResponseRequest = kj::mv(request);
  autoResponseResponse = kj::mv(response);
}

void HibernationManagerImpl::unsetWebSocketAutoResponse() {
  autoResponseRequest = nullptr;
  autoResponseResponse = nullptr;
}

kj::Array<kj::byte> HibernationManagerImpl::serializeV8Value(
    v8::Local<v8::Value> value,
    v8::Isolate* isolate) {
  jsg::Serializer serializer(isolate, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

void HibernationManagerImpl::hibernateWebSockets(
    jsg::Lock& js,
    v8::Isolate* isolate,
    IoContext& context) {

  for (auto& ws : allWs) {
    KJ_IF_MAYBE(active, ws->activeWebSocket) {
      // We need to serialize the attachment before hibernating.
      KJ_IF_MAYBE(attachment, active->get()->getAttachment()) {
        ws->attachment = serializeV8Value(*attachment, isolate);
      }
    }
    ws->activeWebSocket = nullptr;
  }
}

void HibernationManagerImpl::dropHibernatableWebSocket(HibernatableWebSocket& hib) {
  removeFromAllWs(hib);
}

inline void HibernationManagerImpl::removeFromAllWs(HibernatableWebSocket& hib) {
  auto& node = KJ_REQUIRE_NONNULL(hib.node);
  allWs.erase(node);
}

kj::Promise<void> HibernationManagerImpl::readLoop(HibernatableWebSocket& hib) {
  // Like the api::WebSocket readLoop(), but we dispatch different types of events.
  auto& ws = *hib.ws;
  return ws.receive()
      .then([this, &hib, &ws] (kj::WebSocket::Message&& message) mutable -> kj::Promise<void> {
    // TODO(now): Within CustomEvent impl run, when we pass a reference to this hibernation manager,
    // we'll want to assign the hib manager ref to the Worker::Actor so that JS can access it elsewhere.

    auto unhibernate = true;
    KJ_IF_MAYBE (req, autoResponseRequest) {
      // If we have an autoResponseRequest set, we must have
      // autoResponseResponse also.
      KJ_ASSERT_NONNULL(autoResponseResponse);
      KJ_SWITCH_ONEOF(message) {
        KJ_CASE_ONEOF(text, kj::String) {
          if (text == *req) {
            unhibernate = false;
            kj::Duration time =
                static_cast<int64_t>(api::dateNow()) * kj::SECONDS;
            hib.autoResponseTimestamp = kj::UNIX_EPOCH + time;
            KJ_IF_MAYBE (response, autoResponseResponse) {
              ws.send(kj::str(response));
            }
          }
        }
        KJ_CASE_ONEOF_DEFAULT {}
      }
    }
    if (hib.activeWebSocket == nullptr && unhibernate) {
      // If we are currently hibernating, we need to wake up.
      // TODO(now): We need to get access to an Isolate so we can deserialize `attachment` and
      //            pass a jsg::Lock& to unhibernate.
      // TODO(now): We can't create the api::WebSocket at this point since we aren't guaranteed to
      // have an Isolate. We should create and assign the `activeWebSocket` when running inside the
      // CustomEvent instead.
    }

    return readLoop(hib);
  });
}

}; // namespace workerd
