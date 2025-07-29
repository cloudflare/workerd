#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/io-channels.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {

class Fetcher;
class DurableObjectClass;

// JS stub pointing to a remote Worker loaded using WorkerLoader. This is not a stub for a specific
// entrypoint, but instead the entire Worker, allowing the caller to call any entrypoint (and
// specify arbitrary props).
class WorkerStub: public jsg::Object {
 public:
  WorkerStub(IoOwn<WorkerStubChannel> channel): channel(kj::mv(channel)) {}

  struct EntrypointOptions {
    jsg::Optional<jsg::JsObject> props;

    JSG_STRUCT(props);
  };

  jsg::Ref<Fetcher> getEntrypoint(jsg::Lock& js,
      jsg::Optional<kj::Maybe<kj::String>> name,
      jsg::Optional<EntrypointOptions> options);
  jsg::Ref<DurableObjectClass> getDurableObjectClass(jsg::Lock& js,
      jsg::Optional<kj::Maybe<kj::String>> name,
      jsg::Optional<EntrypointOptions> options);

  JSG_RESOURCE_TYPE(WorkerStub) {
    JSG_METHOD(getEntrypoint);
    JSG_METHOD(getDurableObjectClass);
  }

 private:
  IoOwn<WorkerStubChannel> channel;
};

// JS interface for worker loader binding.
class WorkerLoader: public jsg::Object {
 public:
  // Create a WorkerLoader backed by the given I/O channel.
  //
  // `compatDateValidation` will differ between workerd vs. production.
  explicit WorkerLoader(uint channel, CompatibilityDateValidation compatDateValidation)
      : channel(channel),
        compatDateValidation(compatDateValidation) {}

  struct Module {
    // Exactly one must be filled in.
    jsg::Optional<kj::String> js;               // ES module
    jsg::Optional<kj::String> cjs;              // Common JS module
    jsg::Optional<kj::String> text;             // text blob, imports as a string
    jsg::Optional<kj::Array<const byte>> data;  // byte blob, imports as ArrayBuffer
    jsg::Optional<jsg::Value> json;             // arbitrary JS value, will be serialized to JSON
                                                // and then parsed again when imported
    jsg::Optional<kj::String> py;               // Python module

    JSG_STRUCT(js, cjs, text, data, json, py);

    // HACK: When we serialize the JSON in extractSource() we need to place the owned kj::String
    //   somewhere since Worker::Script::Source only gets a kj::StringPtr.
    kj::Maybe<kj::String> serializedJson;
  };

  struct WorkerCode {
    kj::String compatibilityDate;
    jsg::Optional<kj::Array<kj::String>> compatibilityFlags;
    jsg::Optional<bool> allowExperimental = false;

    kj::String mainModule;

    // Modules are specified as an object mapping names to content. If the content is just a
    // string, an ES module is assumed. If it's an object, the type of module is determined
    // based on which property is set.
    jsg::Dict<kj::OneOf<Module, kj::String>> modules;

    // Any RPC-serializable value!
    jsg::Optional<jsg::JsRef<jsg::JsObject>> env;

    // `Fetcher` (e.g. service binding) representing the loaded worker's global outbound.
    //
    // If omitted, inherit the current worker's global outbound.
    //
    // If `null`, block the global outbound (all requests throw errors).
    jsg::Optional<kj::Maybe<jsg::Ref<Fetcher>>> globalOutbound;

    // TODO(someday): cache API outbound?

    // TODO(someday): Support specifying a list of tail workers. These should work similarly to
    //   globalOutbound.

    JSG_STRUCT(compatibilityDate,
        compatibilityFlags,
        allowExperimental,
        mainModule,
        modules,
        env,
        globalOutbound);
  };

  jsg::Ref<WorkerStub> get(
      jsg::Lock& js, kj::String name, jsg::Function<jsg::Promise<WorkerCode>()> getCode);

  JSG_RESOURCE_TYPE(WorkerLoader) {
    JSG_METHOD(get);
  }

 private:
  uint channel;
  CompatibilityDateValidation compatDateValidation;

  static Worker::Script::Source extractSource(jsg::Lock& js, WorkerCode& code);
  static kj::Own<CompatibilityFlags::Reader> extractCompatFlags(
      jsg::Lock& js, WorkerCode& code, CompatibilityDateValidation compatDateValidation);

  kj::Promise<kj::Own<const Worker>> startWorker(
      Worker::Script::Source extractedSource, CompatibilityFlags::Reader compatibilityFlags);
};

#define EW_WORKER_LOADER_ISOLATE_TYPES                                                             \
  api::WorkerStub, api::WorkerStub::EntrypointOptions, api::WorkerLoader,                          \
      api::WorkerLoader::Module, api::WorkerLoader::WorkerCode

}  // namespace workerd::api
