#include "dynamic-isolate.h"

#include <workerd/api/http.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <capnp/message.h>

namespace workerd::api {

class DynamicIsolateOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  DynamicIsolateOutgoingFactory(kj::Own<IoChannelFactory::SubrequestChannel> channel)
      : channel(kj::mv(channel)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    auto& context = IoContext::current();

    return context.getMetrics().wrapSubrequestClient(context.getSubrequest(
        [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
      return channel->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
    },
        {.inHouse = true,
          .wrapMetrics = true,
          .operationName = kj::ConstString("dynamic_isolate_subrequest"_kjc)}));
  }

 private:
  kj::Own<IoChannelFactory::SubrequestChannel> channel;
};

jsg::Ref<Fetcher> DynamicIsolate::getEntrypoint(jsg::Lock& js,
    jsg::Optional<kj::Maybe<kj::String>> name,
    jsg::Optional<EntrypointOptions> options) {
  Frankenvalue props;

  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(p, o.props) {
      props = Frankenvalue::fromJs(js, p);
    }
  }

  kj::Maybe<kj::String> entrypointName;
  KJ_IF_SOME(n, name) {
    KJ_IF_SOME(n2, n) {
      if (n2 != "default"_kj) {
        entrypointName = kj::mv(n2);
      }
    }
  }

  kj::Own<Fetcher::OutgoingFactory> factory = kj::heap<DynamicIsolateOutgoingFactory>(
      channel->getEntrypoint(kj::mv(entrypointName), kj::mv(props)));

  return js.alloc<Fetcher>(
      IoContext::current().addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

jsg::Ref<ActorClass> DynamicIsolate::getActorClass(jsg::Lock& js,
    jsg::Optional<kj::Maybe<kj::String>> name,
    jsg::Optional<EntrypointOptions> options) {
  KJ_UNIMPLEMENTED("TODO(now): DynamicIsolate::getActorClass()");
}

jsg::Ref<DynamicIsolate> DynamicIsolateLoader::get(
    jsg::Lock& js, kj::String name, jsg::Function<jsg::Promise<WorkerCode>()> getCode) {
  auto& ioctx = IoContext::current();

  auto reenterAndGetCode =
      ioctx.makeReentryCallback([getCode = kj::mv(getCode)](jsg::Lock& js) mutable {
    return getCode(js).then(js, [](jsg::Lock& js, WorkerCode code) -> DynamicIsolateSource {
      auto extractedSource = extractSource(js, code);
      auto ownCompatFlags = extractCompatFlags(js, code);
      CompatibilityFlags::Reader compatFlags = *ownCompatFlags;

      // TODO(now): Only take ownership of the non-handles from `code`.
      code.env = kj::none;
      code.globalOutbound = kj::none;
      code.cacheApiOutbound = kj::none;

      return {.source = kj::mv(extractedSource),
        .compatibilityFlags = compatFlags,
        .ownContent = ownCompatFlags.attach(kj::mv(code))};
    });
  });

  auto isolateChannel =
      ioctx.getIoChannelFactory().loadIsolate(channel, kj::mv(name), kj::mv(reenterAndGetCode));

  return js.alloc<DynamicIsolate>(ioctx.addObject(kj::mv(isolateChannel)));
}

Worker::Script::Source DynamicIsolateLoader::extractSource(jsg::Lock& js, WorkerCode& code) {
  JSG_REQUIRE(code.modules.fields.size() > 0, TypeError,
      "Dynamic Worker code must contain at least one module.");

  auto modules = KJ_MAP(module, code.modules.fields) -> Worker::Script::Module {
    uint fieldCount = (module.value.js == kj::none) + (module.value.cjs == kj::none) +
        (module.value.text == kj::none) + (module.value.data == kj::none) +
        (module.value.json == kj::none);
    JSG_REQUIRE(fieldCount == 1, TypeError,
        "Each module must contain exactly one of 'js', 'cjs', 'text', 'data', or 'json'. "
        "Module '",
        module.name, "' contained ", fieldCount, " properties.");

    return {.name = module.name, .content = [&]() -> Worker::Script::ModuleContent {
      KJ_IF_SOME(js, module.value.js) {
        return Worker::Script::EsModule{.body = js};
      } else KJ_IF_SOME(cjs, module.value.cjs) {
        return Worker::Script::CommonJsModule{.body = cjs};
      } else KJ_IF_SOME(text, module.value.text) {
        return Worker::Script::TextModule{.body = text};
      } else KJ_IF_SOME(data, module.value.data) {
        return Worker::Script::DataModule{.body = data};
      } else KJ_IF_SOME(json, module.value.json) {
        kj::StringPtr serialized =
            module.value.serializedJson.emplace(js.serializeJson(kj::mv(json)));
        return Worker::Script::JsonModule{.body = serialized};
      } else {
        KJ_UNREACHABLE;
      }
    }()};
  };

  return Worker::Script::ModulesSource{
    .mainModule = code.mainModule,
    .modules = kj::mv(modules),

    .isPython = false,  // TODO(someday): Support Python.
  };
}

kj::Own<CompatibilityFlags::Reader> DynamicIsolateLoader::extractCompatFlags(
    jsg::Lock& js, WorkerCode& code) {
  if (FeatureFlags::get(js).getWorkerdExperimental()) {
    JSG_REQUIRE(!code.allowExperimental, Error,
        "'allowExperimental' is only allowed when the calling worker has the 'experimental' "
        "compat flag set.");
  }

  kj::ArrayPtr<const kj::String> compatFlags;
  KJ_IF_SOME(f, code.compatibilityFlags) {
    compatFlags = f;
  }

  capnp::word scratch[capnp::sizeInWords<CompatibilityFlags>() + 4];
  capnp::MallocMessageBuilder compatFlagsMessage(scratch);
  auto compatFlagsBuilder = compatFlagsMessage.getRoot<CompatibilityFlags>();

  SimpleWorkerErrorReporter errorReporter;

  // TODO(now): This should depend on whether we're in workerd or prod.
  CompatibilityDateValidation compatDateValidation = CompatibilityDateValidation::CODE_VERSION;

  compileCompatibilityFlags(code.compatibilityDate, compatFlags, compatFlagsBuilder, errorReporter,
      code.allowExperimental, compatDateValidation);

  if (!errorReporter.errors.empty()) {
    JSG_FAIL_REQUIRE(Error, errorReporter.errors.front());
  }

  return capnp::clone(compatFlagsBuilder.asReader());
}

}  // namespace workerd::api
