// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "stored-value.h"

#include "io-context.h"

namespace workerd {

namespace {

// Return the id of the current actor (or the empty string if there is no current actor).
kj::Maybe<kj::String> getCurrentActorId() {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    KJ_IF_SOME(actor, ioContext.getActor()) {
      KJ_SWITCH_ONEOF(actor.getId()) {
        KJ_CASE_ONEOF(s, kj::String) {
          return kj::heapString(s);
        }
        KJ_CASE_ONEOF(actorId, kj::Own<ActorIdFactory::ActorId>) {
          return actorId->toString();
        }
      }
      KJ_UNREACHABLE;
    }
  }
  return kj::none;
}

}  // namespace

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, const jsg::JsValue& value) {
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
      });
  serializer.write(js, value);
  auto released = serializer.release();
  return kj::mv(released.data);
}

jsg::JsValue deserializeV8Value(
    jsg::Lock& js, kj::ArrayPtr<const char> key, kj::ArrayPtr<const kj::byte> buf) {

  KJ_ASSERT(buf.size() > 0, "unexpectedly empty value buffer", key);
  try {
    // The js.tryCatch will handle the normal exception path. We wrap this in an
    // additional try/catch in case the js.tryCatch hits an exception that is
    // terminal for the isolate, causing exception to be rethrown, in which case
    // we throw a kj::Exception wrapping a jsg.Error.
    return js.tryCatch([&]() -> jsg::JsValue {
      jsg::Deserializer::Options options{};
      if (buf[0] != 0xFF) {
        // When Durable Objects was first released, it did not properly write headers when serializing
        // to storage. If we find that the header is missing (as indicated by the first byte not being
        // 0xFF), it's safe to assume that the data was written at the only serialization version we
        // used during that early time period, so we explicitly set that version here.
        options.version = 13;
        options.readHeader = false;
      }

      jsg::Deserializer deserializer(js, buf, kj::none, kj::none, options);

      return deserializer.readValue(js);
    }, [&](jsg::Value&& exception) mutable -> jsg::JsValue {
      // If we do hit a deserialization error, we log information that will be helpful in
      // understanding the problem but that won't leak too much about the customer's data. We
      // include the key (to help find the data in the database if it hasn't been deleted), the
      // length of the value, and the first three bytes of the value (which is just the v8-internal
      // version header and the tag that indicates the type of the value, but not its contents).
      kj::String actorId = getCurrentActorId().orDefault([]() { return kj::String(); });
      KJ_FAIL_ASSERT("actor storage deserialization failed", "failed to deserialize stored value",
          actorId, exception.getHandle(js), key, buf.size(),
          buf.first(std::min(static_cast<size_t>(3), buf.size())));
    });
  } catch (jsg::JsExceptionThrown&) {
    // We can occasionally hit an isolate termination here -- we prefix the error with jsg to avoid
    // counting it against our internal storage error metrics but also throw a KJ exception rather
    // than a jsExceptionThrown error to avoid confusing the normal termination handling code.
    // We don't expect users to ever actually see this error.
    JSG_FAIL_REQUIRE(Error,
        "isolate terminated while deserializing value from Durable Object "
        "storage; contact us if you're wondering why you're seeing this");
  }
}

}  // namespace workerd
