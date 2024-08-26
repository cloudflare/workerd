// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "rtti.h"

#include <kj/map.h>
#include <capnp/serialize-packed.h>

#include <workerd/api/actor.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/kv.h>
#include <workerd/api/modules.h>
#include <workerd/api/queue.h>
#include <workerd/api/r2.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/sockets.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/trace.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/node/node.h>
#include <workerd/jsg/modules.capnp.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/compatibility-date.h>

#include <cloudflare/cloudflare.capnp.h>

#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
#include <workerd/api/gpu/gpu.h>
#else
#define EW_WEBGPU_ISOLATE_TYPES
#endif

#define EW_TYPE_GROUP_FOR_EACH(F)                                                                  \
  F("dom-exception", jsg::DOMException)                                                            \
  F("global-scope", EW_GLOBAL_SCOPE_ISOLATE_TYPES)                                                 \
  F("durable-objects", EW_ACTOR_ISOLATE_TYPES)                                                     \
  F("durable-objects-state", EW_ACTOR_STATE_ISOLATE_TYPES)                                         \
  F("analytics-engine", EW_ANALYTICS_ENGINE_ISOLATE_TYPES)                                         \
  F("basics", EW_BASICS_ISOLATE_TYPES)                                                             \
  F("blob", EW_BLOB_ISOLATE_TYPES)                                                                 \
  F("cache", EW_CACHE_ISOLATE_TYPES)                                                               \
  F("crypto", EW_CRYPTO_ISOLATE_TYPES)                                                             \
  F("encoding", EW_ENCODING_ISOLATE_TYPES)                                                         \
  F("events", EW_EVENTS_ISOLATE_TYPES)                                                             \
  F("form-data", EW_FORMDATA_ISOLATE_TYPES)                                                        \
  F("html-rewriter", EW_HTML_REWRITER_ISOLATE_TYPES)                                               \
  F("http", EW_HTTP_ISOLATE_TYPES)                                                                 \
  F("hyperdrive", EW_HYPERDRIVE_ISOLATE_TYPES)                                                     \
  F("unsafe", EW_UNSAFE_ISOLATE_TYPES)                                                             \
  F("memory-cache", EW_MEMORY_CACHE_ISOLATE_TYPES)                                                 \
  F("pyodide", EW_PYODIDE_ISOLATE_TYPES)                                                           \
  F("kv", EW_KV_ISOLATE_TYPES)                                                                     \
  F("queue", EW_QUEUE_ISOLATE_TYPES)                                                               \
  F("r2-admin", EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES)                                             \
  F("r2", EW_R2_PUBLIC_BETA_ISOLATE_TYPES)                                                         \
  F("worker-rpc", EW_WORKER_RPC_ISOLATE_TYPES)                                                     \
  F("scheduled", EW_SCHEDULED_ISOLATE_TYPES)                                                       \
  F("streams", EW_STREAMS_ISOLATE_TYPES)                                                           \
  F("trace", EW_TRACE_ISOLATE_TYPES)                                                               \
  F("url", EW_URL_ISOLATE_TYPES)                                                                   \
  F("url-standard", EW_URL_STANDARD_ISOLATE_TYPES)                                                 \
  F("url-pattern", EW_URLPATTERN_ISOLATE_TYPES)                                                    \
  F("websocket", EW_WEBSOCKET_ISOLATE_TYPES)                                                       \
  F("sql", EW_SQL_ISOLATE_TYPES)                                                                   \
  F("sockets", EW_SOCKETS_ISOLATE_TYPES)                                                           \
  F("node", EW_NODE_ISOLATE_TYPES)                                                                 \
  F("rtti", EW_RTTI_ISOLATE_TYPES)                                                                 \
  F("webgpu", EW_WEBGPU_ISOLATE_TYPES)                                                             \
  F("eventsource", EW_EVENTSOURCE_ISOLATE_TYPES)

namespace workerd::api {

namespace {

struct EncoderModuleRegistryImpl {
  struct CppModuleContents {
    CppModuleContents(kj::String structureName): structureName(kj::mv(structureName)) {}

    kj::String structureName;
  };
  struct TypeScriptModuleContents {
    TypeScriptModuleContents(kj::StringPtr tsDeclarations): tsDeclarations(tsDeclarations) {}

    kj::StringPtr tsDeclarations;
  };
  struct ModuleInfo {
    ModuleInfo(kj::StringPtr specifier,
        jsg::ModuleType type,
        kj::OneOf<CppModuleContents, TypeScriptModuleContents> contents)
        : specifier(specifier),
          type(type),
          contents(kj::mv(contents)) {}

    kj::StringPtr specifier;
    jsg::ModuleType type;
    kj::OneOf<CppModuleContents, TypeScriptModuleContents> contents;
  };

  void addBuiltinBundle(
      jsg::Bundle::Reader bundle, kj::Maybe<jsg::ModuleRegistry::Type> maybeFilter = kj::none) {
    for (auto module: bundle.getModules()) {
      if (module.getType() == maybeFilter.orDefault(module.getType())) addBuiltinModule(module);
    }
  }

  void addBuiltinModule(jsg::Module::Reader module) {
    TypeScriptModuleContents contents(module.getTsDeclaration());
    ModuleInfo info(module.getName(), module.getType(), kj::mv(contents));
    modules.add(kj::mv(info));
  }

  template <typename T>
  void addBuiltinModule(kj::StringPtr specifier,
      jsg::ModuleRegistry::Type type = jsg::ModuleRegistry::Type::BUILTIN) {
    auto structureName = jsg::fullyQualifiedTypeName(typeid(T));
    CppModuleContents contents(kj::mv(structureName));
    ModuleInfo info(specifier, type, kj::mv(contents));
    modules.add(kj::mv(info));
  }

  kj::Vector<ModuleInfo> modules;
};

CompatibilityFlags::Reader compileFlags(capnp::MessageBuilder &message,
    kj::StringPtr compatDate,
    bool experimental,
    kj::ArrayPtr<kj::String> compatFlags) {
  // Based on src/workerd/io/compatibility-date-test.c++
  auto orphanage = message.getOrphanage();
  auto flagListOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(compatFlags.size());
  auto flagList = flagListOrphan.get();
  for (auto i: kj::indices(compatFlags)) {
    flagList.set(i, compatFlags.begin()[i]);
  }

  auto output = message.initRoot<CompatibilityFlags>();
  SimpleWorkerErrorReporter errorReporter;

  compileCompatibilityFlags(compatDate, flagList.asReader(), output, errorReporter, experimental,
      CompatibilityDateValidation::FUTURE_FOR_TEST);

  if (!errorReporter.errors.empty()) {
    // TODO(someday): throw an `AggregateError` containing all errors
    JSG_FAIL_REQUIRE(Error, errorReporter.errors[0]);
  }

  auto reader = output.asReader();
  return kj::mv(reader);
}

CompatibilityFlags::Reader compileAllFlags(capnp::MessageBuilder &message) {
  auto output = message.initRoot<CompatibilityFlags>();
  auto schema = capnp::Schema::from<CompatibilityFlags>();
  auto dynamicOutput = capnp::toDynamic(output);
  for (auto field: schema.getFields()) {
    bool isNode = false;

    kj::StringPtr enableFlagName;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        enableFlagName = annotation.getValue().getText();
        // Exclude nodejs_compat, since the type generation scripts don't support node:* imports
        // TODO: Figure out typing for node compat
        isNode = enableFlagName == "nodejs_compat" || enableFlagName == "nodejs_compat_v2";
      }
    }

    dynamicOutput.set(field, !isNode);
  }
  auto reader = output.asReader();
  return kj::mv(reader);
}

struct TypesEncoder {
public:
  TypesEncoder(): compatFlags(kj::heapArray<kj::String>(0)) {}
  TypesEncoder(kj::String compatDate, kj::Array<kj::String> compatFlags)
      : compatDate(kj::mv(compatDate)),
        compatFlags(kj::mv(compatFlags)) {}

  kj::Array<byte> encode() {
    capnp::MallocMessageBuilder flagsMessage;
    CompatibilityFlags::Reader flags;
    KJ_IF_SOME(date, compatDate) {
      flags = compileFlags(flagsMessage, date, true, compatFlags);
    } else {
      flags = compileAllFlags(flagsMessage);
    }
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<jsg::rtti::StructureGroups>();

    // Encode RTTI structures
    auto builder = jsg::rtti::Builder(flags);

#define EW_TYPE_GROUP_COUNT(Name, Types) groupsSize++;
#define EW_TYPE_GROUP_WRITE(Name, Types) writeGroup<Types>(groups, builder, Name);

    unsigned int groupsSize = 0;
    EW_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_COUNT)
    auto groups = root.initGroups(groupsSize);
    groupsIndex = 0;
    EW_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_WRITE)
    KJ_ASSERT(groupsIndex == groupsSize);

#undef EW_TYPE_GROUP_COUNT
#undef EW_TYPE_GROUP_WRITE

    // Encode modules
    EncoderModuleRegistryImpl registry;
    registerModules(registry, flags);

    unsigned int i = 0;
    auto modulesBuilder = root.initModules(registry.modules.size());
    for (auto moduleBuilder: modulesBuilder) {
      auto &module = registry.modules[i++];
      moduleBuilder.setSpecifier(module.specifier);
      KJ_SWITCH_ONEOF(module.contents) {
        KJ_CASE_ONEOF(contents, EncoderModuleRegistryImpl::CppModuleContents) {
          moduleBuilder.setStructureName(contents.structureName);
        }
        KJ_CASE_ONEOF(contents, EncoderModuleRegistryImpl::TypeScriptModuleContents) {
          moduleBuilder.setTsDeclarations(contents.tsDeclarations);
        }
      }
    }

    auto words = capnp::messageToFlatArray(message);
    auto bytes = words.asBytes();
    return kj::heapArray(bytes);
  }

private:
  template <typename Type>
  void writeStructure(jsg::rtti::Builder<CompatibilityFlags::Reader> &builder,
      capnp::List<jsg::rtti::Structure>::Builder structures) {
    auto reader = builder.structure<Type>();
    structures.setWithCaveats(structureIndex++, reader);
  }

  template <typename... Types>
  void writeGroup(capnp::List<jsg::rtti::StructureGroups::StructureGroup>::Builder &groups,
      jsg::rtti::Builder<CompatibilityFlags::Reader> &builder,
      kj::StringPtr name) {
    auto group = groups[groupsIndex++];
    group.setName(name);

    unsigned int structuresSize = sizeof...(Types);
    auto structures = group.initStructures(structuresSize);
    structureIndex = 0;
    (writeStructure<Types>(builder, structures), ...);
    KJ_ASSERT(structureIndex == structuresSize);
  }

  kj::Maybe<kj::String> compatDate;
  kj::Array<kj::String> compatFlags;

  unsigned int groupsIndex = 0;
  unsigned int structureIndex = 0;
};

}  // namespace

kj::Array<byte> RTTIModule::exportTypes(kj::String compatDate, kj::Array<kj::String> compatFlags) {
  TypesEncoder encoder(kj::mv(compatDate), kj::mv(compatFlags));
  return encoder.encode();
}

kj::Array<byte> RTTIModule::exportExperimentalTypes() {
  TypesEncoder encoder;
  return encoder.encode();
}

}  // namespace workerd::api
