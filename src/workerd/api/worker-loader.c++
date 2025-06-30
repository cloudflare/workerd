#include "worker-loader.h"

#include <workerd/api/actor.h>
#include <workerd/api/http.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <capnp/message.h>

namespace workerd::api {

class WorkerStubEntrypointOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  WorkerStubEntrypointOutgoingFactory(kj::Own<IoChannelFactory::SubrequestChannel> channel)
      : channel(kj::mv(channel)) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String> cfStr) override {
    auto& context = IoContext::current();

    return context.getMetrics().wrapSubrequestClient(context.getSubrequest(
        [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
      return channel->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
    },
        {.inHouse = true,
          .wrapMetrics = true,
          .operationName = kj::ConstString("dynamic_worker_subrequest"_kjc)}));
  }

 private:
  kj::Own<IoChannelFactory::SubrequestChannel> channel;
};

jsg::Ref<Fetcher> WorkerStub::getEntrypoint(jsg::Lock& js,
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

  kj::Own<Fetcher::OutgoingFactory> factory = kj::heap<WorkerStubEntrypointOutgoingFactory>(
      channel->getEntrypoint(kj::mv(entrypointName), kj::mv(props)));

  return js.alloc<Fetcher>(
      IoContext::current().addObject(kj::mv(factory)), Fetcher::RequiresHostAndProtocol::YES, true);
}

jsg::Ref<DurableObjectClass> WorkerStub::getDurableObjectClass(jsg::Lock& js,
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

  return js.alloc<DurableObjectClass>(IoContext::current().addObject(
      channel->getActorClass(kj::mv(entrypointName), kj::mv(props))));
}

class NullGlobalOutboundChannel: public IoChannelFactory::SubrequestChannel {
 public:
  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    JSG_FAIL_REQUIRE(Error,
        "This worker is not permitted to access the internet via global functions like fetch(). "
        "It must use capabilities (such as bindings in 'env') to talk to the outside world.");
  }
};

jsg::Ref<WorkerStub> WorkerLoader::get(
    jsg::Lock& js, kj::String name, jsg::Function<jsg::Promise<WorkerCode>()> getCode) {
  auto& ioctx = IoContext::current();

  auto reenterAndGetCode = ioctx.makeReentryCallback(
      [&ioctx, getCode = kj::mv(getCode), compatDateValidation = compatDateValidation](
          jsg::Lock& js) mutable {
    return getCode(js).then(
        js, [&ioctx, compatDateValidation](jsg::Lock& js, WorkerCode code) -> DynamicWorkerSource {
      auto extractedSource = extractSource(js, code);
      auto ownCompatFlags = extractCompatFlags(js, code, compatDateValidation);
      CompatibilityFlags::Reader compatFlags = *ownCompatFlags;

      Frankenvalue env;
      KJ_IF_SOME(codeEnv, code.env) {
        env = Frankenvalue::fromJs(js, codeEnv.getHandle(js));
      }

      kj::Own<IoChannelFactory::SubrequestChannel> globalOutbound;
      KJ_IF_SOME(maybeOut, code.globalOutbound) {
        KJ_IF_SOME(out, maybeOut) {
          globalOutbound = out->getSubrequestChannel(ioctx);
        } else {
          globalOutbound = kj::refcounted<NullGlobalOutboundChannel>();
        }
      } else {
        // Inherit the calling worker's global outbound channel.
        globalOutbound =
            ioctx.getIoChannelFactory().getSubrequestChannel(IoContext::NULL_CLIENT_CHANNEL);
      }

      return {.source = kj::mv(extractedSource),
        .compatibilityFlags = compatFlags,
        .env = kj::mv(env),
        .globalOutbound = kj::mv(globalOutbound),
        .ownContent = ownCompatFlags.attach(kj::mv(code.modules), kj::mv(code.mainModule))};
    });
  });

  auto isolateChannel =
      ioctx.getIoChannelFactory().loadIsolate(channel, kj::mv(name), kj::mv(reenterAndGetCode));

  return js.alloc<WorkerStub>(ioctx.addObject(kj::mv(isolateChannel)));
}

Worker::Script::Source WorkerLoader::extractSource(jsg::Lock& js, WorkerCode& code) {
  JSG_REQUIRE(code.modules.fields.size() > 0, TypeError,
      "Dynamic Worker code must contain at least one module.");

  auto modules = KJ_MAP(entry, code.modules.fields) -> Worker::Script::Module {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(text, kj::String) {
        return {
          .name = entry.name,
          .content = Worker::Script::EsModule{.body = text},
        };
      }
      KJ_CASE_ONEOF(module, Module) {
        uint fieldCount = (module.js != kj::none) + (module.cjs != kj::none) +
            (module.text != kj::none) + (module.data != kj::none) + (module.json != kj::none);
        JSG_REQUIRE(fieldCount == 1, TypeError,
            "Each module must contain exactly one of 'js', 'cjs', 'text', 'data', or 'json'. "
            "Module '",
            entry.name, "' contained ", fieldCount, " properties.");

        return {.name = entry.name, .content = [&]() -> Worker::Script::ModuleContent {
          KJ_IF_SOME(js, module.js) {
            return Worker::Script::EsModule{.body = js};
          } else KJ_IF_SOME(cjs, module.cjs) {
            return Worker::Script::CommonJsModule{.body = cjs};
          } else KJ_IF_SOME(text, module.text) {
            return Worker::Script::TextModule{.body = text};
          } else KJ_IF_SOME(data, module.data) {
            return Worker::Script::DataModule{.body = data};
          } else KJ_IF_SOME(json, module.json) {
            kj::StringPtr serialized =
                module.serializedJson.emplace(js.serializeJson(kj::mv(json)));
            return Worker::Script::JsonModule{.body = serialized};
          } else {
            KJ_UNREACHABLE;
          }
        }()};
      }
    }
    KJ_UNREACHABLE;
  };

  return Worker::Script::ModulesSource{
    .mainModule = code.mainModule,
    .modules = kj::mv(modules),

    .isPython = false,  // TODO(someday): Support Python.
  };
}

kj::Own<CompatibilityFlags::Reader> WorkerLoader::extractCompatFlags(
    jsg::Lock& js, WorkerCode& code, CompatibilityDateValidation compatDateValidation) {
  bool allowExperimental = code.allowExperimental.orDefault(false);
  if (!FeatureFlags::get(js).getWorkerdExperimental()) {
    JSG_REQUIRE(!allowExperimental, Error,
        "'allowExperimental' is only allowed when the calling worker has the 'experimental' "
        "compat flag set.");
  }

  kj::ArrayPtr<const kj::String> compatFlags;
  KJ_IF_SOME(f, code.compatibilityFlags) {
    compatFlags = f;
  }

  capnp::word scratch[capnp::sizeInWords<CompatibilityFlags>() + 4]{};
  capnp::MallocMessageBuilder compatFlagsMessage(scratch);
  auto compatFlagsBuilder = compatFlagsMessage.getRoot<CompatibilityFlags>();

  SimpleWorkerErrorReporter errorReporter;

  compileCompatibilityFlags(code.compatibilityDate, compatFlags, compatFlagsBuilder, errorReporter,
      allowExperimental, compatDateValidation);

  if (!errorReporter.errors.empty()) {
    JSG_FAIL_REQUIRE(Error, errorReporter.errors.front());
  }

  return capnp::clone(compatFlagsBuilder.asReader());
}

}  // namespace workerd::api
