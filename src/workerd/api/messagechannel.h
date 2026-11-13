#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

// cloudflare-internal:messagechannel, through which built-in modules like node:worker_threads
// reach MessageChannel and MessagePort whether or not the expose_global_message_channel compat
// flag makes them globals. Both classes are implemented in TypeScript
// (src/per_isolate/messagechannel/); the per-isolate bootstrap loads them in every context, and
// this module hands out the bootstrap's exports.
class MessageChannelModule final: public jsg::Object {
 public:
  MessageChannelModule() = default;
  MessageChannelModule(jsg::Lock&, const jsg::Url&) {}

  jsg::JsValue getMessageChannel(jsg::Lock& js);
  jsg::JsValue getMessagePort(jsg::Lock& js);

  // Stateless module object, safe to recreate for a worker started from a snapshot.
  static constexpr bool jsgSnapshotStateless = true;

  JSG_RESOURCE_TYPE(MessageChannelModule) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(MessageChannel, getMessageChannel);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(MessagePort, getMessagePort);
  }
};

template <class Registry>
void registerMessageChannelModule(Registry& registry, auto featureFlags) {
  registry.template addBuiltinModule<MessageChannelModule>(
      "cloudflare-internal:messagechannel", workerd::jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
kj::Own<jsg::modules::ModuleBundle> getInternalMessageChannelModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  static const auto kSpecifier = "cloudflare-internal:messagechannel"_url;
  builder.addObject<MessageChannelModule, TypeWrapper>(kSpecifier);
  return builder.finish();
}

}  // namespace workerd::api

#define EW_MESSAGECHANNEL_ISOLATE_TYPES api::MessageChannelModule
