#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {

class Fetcher;
class ActorClass;

// JS interface for a dynamic isolate.
class DynamicIsolate: public jsg::Object {
 public:
  DynamicIsolate(IoOwn<DynamicIsolateChannel> channel): channel(kj::mv(channel)) {}

  struct EntrypointOptions {
    jsg::Optional<jsg::JsObject> props;

    JSG_STRUCT(props);
  };

  jsg::Ref<Fetcher> getEntrypoint(jsg::Lock& js,
      jsg::Optional<kj::Maybe<kj::String>> name,
      jsg::Optional<EntrypointOptions> options);
  jsg::Ref<ActorClass> getActorClass(jsg::Lock& js,
      jsg::Optional<kj::Maybe<kj::String>> name,
      jsg::Optional<EntrypointOptions> options);

  JSG_RESOURCE_TYPE(DynamicIsolate) {
    JSG_METHOD(getEntrypoint);
    JSG_METHOD(getActorClass);
  }

 private:
  IoOwn<DynamicIsolateChannel> channel;
};

// JS interface for a dynamic isolate loader binding.
class DynamicIsolateLoader: public jsg::Object {
 public:
  explicit DynamicIsolateLoader(uint channel): channel(channel) {}

  struct Module {
    // Exactly one must be filled in.
    jsg::Optional<kj::String> js;
    jsg::Optional<kj::String> cjs;
    jsg::Optional<kj::String> text;
    jsg::Optional<kj::Array<const byte>> data;
    jsg::Optional<jsg::Value> json;

    JSG_STRUCT(js, cjs, text, data, json);

    // HACK: When we serialize the JSON in extractSource() we need to place the owned kj::String
    //   somewhere since Worker::Script::Source only gets a kj::StringPtr.
    kj::Maybe<kj::String> serializedJson;
  };

  struct WorkerCode {
    kj::String compatibilityDate;
    jsg::Optional<kj::Array<kj::String>> compatibilityFlags;
    bool allowExperimental = false;

    kj::String mainModule;
    jsg::Dict<Module> modules;

    // Any RPC-serializable value!
    jsg::Optional<jsg::JsValue> env;

    jsg::Optional<jsg::Ref<Fetcher>> globalOutbound;

    jsg::Optional<jsg::Ref<Fetcher>> cacheApiOutbound;

    // TODO(someday): Tails?

    JSG_STRUCT(compatDate,
        compatFlags,
        allowExperimental,
        mainModule,
        modules,
        env,
        globalOutbound,
        cacheApiOutbound);
  };

  jsg::Ref<DynamicIsolate> get(
      jsg::Lock& js, kj::String name, jsg::Function<jsg::Promise<WorkerCode>()> getCode);

  JSG_RESOURCE_TYPE(DynamicIsolateLoader) {
    JSG_METHOD(get);
  }

 private:
  uint channel;

  static Worker::Script::Source extractSource(jsg::Lock& js, WorkerCode& code);
  static kj::Own<CompatibilityFlags::Reader> extractCompatFlags(jsg::Lock& js, WorkerCode& code);

  kj::Promise<kj::Own<const Worker>> startWorker(
      Worker::Script::Source extractedSource, CompatibilityFlags::Reader compatibilityFlags);
};

}  // namespace workerd::api
