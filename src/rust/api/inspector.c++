// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "inspector.h"

#include <workerd/io/worker.h>
#include <workerd/util/thread-scopes.h>

#include <v8-inspector.h>

#include <kj/encoding.h>
#include <kj/string.h>
#include <kj/vector.h>

#include <memory>

namespace workerd::rust::api {

namespace {

class InspectorSessionImpl final: public InspectorSession {
 public:
  explicit InspectorSessionImpl(v8_inspector::V8Inspector& inspector)
      : session(inspector.connect(
            // Context group 1 matches the id used in setupContext()'s contextCreated() call (see
            // worker.c++).
            1,
            &channel,
            v8_inspector::StringView(),
            v8_inspector::V8Inspector::kFullyTrusted)) {}
  KJ_DISALLOW_COPY_AND_MOVE(InspectorSessionImpl);

  void dispatch(::rust::Str message) {
    // An 8-bit StringView is Latin-1, so only ASCII can be passed as is.
    auto bytes = kj::arrayPtr(reinterpret_cast<const kj::byte*>(message.data()), message.size());
    bool isAscii = true;
    for (auto byte: bytes) {
      if (byte >= 0x80) {
        isAscii = false;
        break;
      }
    }
    if (isAscii) {
      session->dispatchProtocolMessage(v8_inspector::StringView(bytes.begin(), bytes.size()));
    } else {
      auto utf16 = kj::encodeUtf16(kj::arrayPtr(message.data(), message.size()));
      session->dispatchProtocolMessage(
          v8_inspector::StringView(reinterpret_cast<const uint16_t*>(utf16.begin()), utf16.size()));
    }
  }

  ::rust::Vec<::rust::String> takeMessages() {
    ::rust::Vec<::rust::String> result;
    std::swap(result, outgoing);
    return result;
  }

 private:
  // Buffers the responses and notifications the V8 inspector sends, rather than handing them to
  // JavaScript, so that the V8 inspector is never reentered from its own Channel. The Rust side
  // collects them with takeMessages() and delivers them to JavaScript a batch at a time.
  class Channel final: public v8_inspector::V8Inspector::Channel {
   public:
    explicit Channel(::rust::Vec<::rust::String>& outgoing): outgoing(outgoing) {}

    void sendResponse(int callId, std::unique_ptr<v8_inspector::StringBuffer> message) override {
      outgoing.push_back(toString(message->string()));
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
      outgoing.push_back(toString(message->string()));
    }
    void flushProtocolNotifications() override {}

   private:
    static ::rust::String toString(v8_inspector::StringView view) {
      if (view.is8Bit()) {
        return ::rust::String::latin1(
            reinterpret_cast<const char*>(view.characters8()), view.length());
      }
      return ::rust::String::lossy(
          reinterpret_cast<const char16_t*>(view.characters16()), view.length());
    }

    ::rust::Vec<::rust::String>& outgoing;
  };

  // Declared in the order of their dependencies, so each outlives the ones after it: the channel
  // refers to `outgoing`, and the session holds a raw pointer to `channel`.
  ::rust::Vec<::rust::String> outgoing;
  Channel channel{outgoing};
  std::unique_ptr<v8_inspector::V8InspectorSession> session;
};

}  // namespace

bool inspector_is_multi_tenant_process() {
  return isMultiTenantProcess();
}

bool inspector_isolate_has_v8_inspector(v8::Isolate* isolate) {
  return Worker::Isolate::from(isolate).tryGetV8Inspector() != kj::none;
}

kj::Own<InspectorSession> inspector_session_connect(v8::Isolate* isolate) {
  // Defense in depth: the `enable_nodejs_inspector_local_dev` flag is `$experimental` (so it cannot
  // be used in Workers deployed to Cloudflare) and the V8 inspector is only created outside
  // multi-tenant processes. The Rust caller reports these cases as errors; they are asserted
  // again here so this code path can never do anything in the production runtime.
  KJ_ASSERT(!isMultiTenantProcess());
  auto& inspector = KJ_ASSERT_NONNULL(Worker::Isolate::from(isolate).tryGetV8Inspector());
  return kj::heap<InspectorSessionImpl>(inspector);
}

void inspector_session_dispatch(InspectorSession& session, ::rust::Str message) {
  static_cast<InspectorSessionImpl&>(session).dispatch(message);
}

::rust::Vec<::rust::String> inspector_session_take_messages(InspectorSession& session) {
  return static_cast<InspectorSessionImpl&>(session).takeMessages();
}

}  // namespace workerd::rust::api
