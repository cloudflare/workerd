#include "worker-loader.h"

#include <workerd/api/actor.h>
#include <workerd/api/http.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <capnp/message.h>

namespace workerd::api {

jsg::Ref<Fetcher> WorkerStub::getEntrypoint(jsg::Lock& js,
    jsg::Optional<kj::Maybe<kj::String>> name,
    jsg::Optional<EntrypointOptions> options) {
  Frankenvalue props;

  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(p, o.props) {
      props = Frankenvalue::fromJs(js, p.getHandle(js));
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

  auto subreqChannel = channel->getEntrypoint(kj::mv(entrypointName), kj::mv(props));
  return js.alloc<Fetcher>(IoContext::current().addObject(kj::mv(subreqChannel)));
}

jsg::Ref<DurableObjectClass> WorkerStub::getDurableObjectClass(jsg::Lock& js,
    jsg::Optional<kj::Maybe<kj::String>> name,
    jsg::Optional<EntrypointOptions> options) {
  Frankenvalue props;

  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(p, o.props) {
      props = Frankenvalue::fromJs(js, p.getHandle(js));
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

      kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> globalOutbound;
      KJ_IF_SOME(maybeOut, code.globalOutbound) {
        KJ_IF_SOME(out, maybeOut) {
          auto channel = out->getSubrequestChannel(ioctx);
          channel->requireAllowsTransfer();
          globalOutbound = kj::mv(channel);
        } else {
          // Application passed `null` to disable internet access. Leave `globalOutbound` as
          // `kj::none`.
        }
      } else {
        // Inherit the calling worker's global outbound channel.
        //
        // Note we don't need to enforce transferrability in this case because if it was the global
        // outbound of the parent, it must be OK to be the global outbound of the child.
        globalOutbound =
            ioctx.getIoChannelFactory().getSubrequestChannel(IoContext::NULL_CLIENT_CHANNEL);
      }

      kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> tailChannels;
      KJ_IF_SOME(tails, code.tails) {
        tailChannels = KJ_MAP(tail, tails) {
          auto channel = tail->getSubrequestChannel(ioctx);
          channel->requireAllowsTransfer();
          return kj::mv(channel);
        };
      }

      kj::Array<kj::Own<IoChannelFactory::SubrequestChannel>> streamingTailChannels;
      KJ_IF_SOME(streamingTails, code.streamingTails) {
        JSG_REQUIRE(code.allowExperimental.orDefault(false), Error,
            "Streaming tail workers are experimental. You must pass the option "
            "'allowExperimental: true' to the worker loader to use them");

        streamingTailChannels = KJ_MAP(tail, streamingTails) {
          auto channel = tail->getSubrequestChannel(ioctx);
          channel->requireAllowsTransfer();
          return kj::mv(channel);
        };
      }

      return {.source = kj::mv(extractedSource),
        .compatibilityFlags = compatFlags,
        .env = kj::mv(env),
        .globalOutbound = kj::mv(globalOutbound),
        .tails = kj::mv(tailChannels),
        .streamingTails = kj::mv(streamingTailChannels),
        .ownContent = ownCompatFlags.attach(kj::mv(code.modules), kj::mv(code.mainModule)),
        .ownContentIsRpcResponse = false};
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
        if (entry.name.endsWith(".py"_kj)) {
          return {
            .name = entry.name,
            .content = Worker::Script::PythonModule{.body = text},
          };
        }

        if (entry.name.endsWith(".js"_kj)) {
          return {
            .name = entry.name,
            .content = Worker::Script::EsModule{.body = text},
          };
        }

        JSG_FAIL_REQUIRE(TypeError,
            "Module name must end with '.js' or '.py' (or the content must be an object ",
            "indicating the type explicitly). Got: ", entry.name);
      }
      KJ_CASE_ONEOF(module, Module) {
        uint fieldCount = (module.js != kj::none) + (module.cjs != kj::none) +
            (module.text != kj::none) + (module.data != kj::none) + (module.json != kj::none) +
            (module.py != kj::none);
        JSG_REQUIRE(fieldCount == 1, TypeError,
            "Each module must contain exactly one of 'js', 'cjs', 'text', 'data', 'json', or 'py'. "
            "Module '",
            entry.name, "' contained ", fieldCount, " properties.");

        return {.name = entry.name, .content = [&]() -> Worker::Script::ModuleContent {
          KJ_IF_SOME(js, module.js) {
            // TODO: this might need typescript transpilation too.
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
          } else KJ_IF_SOME(py, module.py) {
            return Worker::Script::PythonModule{.body = py};
          } else {
            KJ_UNREACHABLE;
          }
        }()};
      }
    }
    KJ_UNREACHABLE;
  };

  bool isPython = code.mainModule.endsWith(".py"_kj);
  // Disallow Python modules when the main module is a JS module, and vice versa.
  for (auto& module: modules) {
    auto isJsModule = module.content.is<Worker::Script::EsModule>() ||
        module.content.is<Worker::Script::CommonJsModule>();
    if (isPython && isJsModule) {
      JSG_FAIL_REQUIRE(TypeError, "Module \"", module.name,
          "\" is a JS module, but the main module is a Python module.");
    }
    auto isPythonModule = module.content.is<Worker::Script::PythonModule>();
    if (!isPython && isPythonModule) {
      JSG_FAIL_REQUIRE(TypeError, "Module \"", module.name,
          "\" is a Python module, but the main module isn't a Python module.");
    }
  }

  return Worker::Script::ModulesSource{
    .mainModule = code.mainModule,
    .modules = kj::mv(modules),
    .isPython = isPython,
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
