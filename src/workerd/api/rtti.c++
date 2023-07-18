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
#include <workerd/api/crypto.h>
#include <workerd/api/encoding.h>
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
#include <workerd/api/streams/standard.h>
#include <workerd/api/trace.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/node/node.h>
#include <workerd/jsg/modules.capnp.h>

#include <cloudflare/cloudflare.capnp.h>

#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
#include <workerd/api/gpu/gpu.h>
#else
#define EW_WEBGPU_ISOLATE_TYPES
#endif

#define EW_TYPE_GROUP_FOR_EACH(F)                                              \
  F("dom-exception", jsg::DOMException)                                        \
  F("global-scope", EW_GLOBAL_SCOPE_ISOLATE_TYPES)                             \
  F("durable-objects", EW_ACTOR_ISOLATE_TYPES)                                 \
  F("durable-objects-state", EW_ACTOR_STATE_ISOLATE_TYPES)                     \
  F("analytics-engine", EW_ANALYTICS_ENGINE_ISOLATE_TYPES)                     \
  F("basics", EW_BASICS_ISOLATE_TYPES)                                         \
  F("blob", EW_BLOB_ISOLATE_TYPES)                                             \
  F("cache", EW_CACHE_ISOLATE_TYPES)                                           \
  F("crypto", EW_CRYPTO_ISOLATE_TYPES)                                         \
  F("encoding", EW_ENCODING_ISOLATE_TYPES)                                     \
  F("form-data", EW_FORMDATA_ISOLATE_TYPES)                                    \
  F("html-rewriter", EW_HTML_REWRITER_ISOLATE_TYPES)                           \
  F("http", EW_HTTP_ISOLATE_TYPES)                                             \
  F("kv", EW_KV_ISOLATE_TYPES)                                                 \
  F("queue", EW_QUEUE_ISOLATE_TYPES)                                           \
  F("r2-admin", EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES)                         \
  F("r2", EW_R2_PUBLIC_BETA_ISOLATE_TYPES)                                     \
  F("scheduled", EW_SCHEDULED_ISOLATE_TYPES)                                   \
  F("streams", EW_STREAMS_ISOLATE_TYPES)                                       \
  F("trace", EW_TRACE_ISOLATE_TYPES)                                           \
  F("url", EW_URL_ISOLATE_TYPES)                                               \
  F("url-standard", EW_URL_STANDARD_ISOLATE_TYPES)                             \
  F("url-pattern", EW_URLPATTERN_ISOLATE_TYPES)                                \
  F("websocket", EW_WEBSOCKET_ISOLATE_TYPES)                                   \
  F("sql", EW_SQL_ISOLATE_TYPES)                                               \
  F("sockets", EW_SOCKETS_ISOLATE_TYPES)                                       \
  F("node", EW_NODE_ISOLATE_TYPES)                                             \
  F("rtti", EW_RTTI_ISOLATE_TYPES)                                             \
  F("webgpu", EW_WEBGPU_ISOLATE_TYPES)

namespace workerd::api {

namespace {

struct EncoderErrorReporterImpl : public Worker::ValidationErrorReporter {
  void addError(kj::String error) override { errors.add(kj::mv(error)); }
  void addHandler(kj::Maybe<kj::StringPtr> exportName,
                  kj::StringPtr type) override {
    KJ_UNREACHABLE;
  }

  kj::Vector<kj::String> errors;
};

struct EncoderModuleRegistryImpl {
  struct CppModuleContents {
    CppModuleContents(kj::String structureName) : structureName(kj::mv(structureName)) {}

    kj::String structureName;
  };
  struct TypeScriptModuleContents {
    TypeScriptModuleContents(kj::StringPtr tsDeclarations) : tsDeclarations(tsDeclarations) {}

    kj::StringPtr tsDeclarations;
  };
  struct ModuleInfo {
    ModuleInfo(kj::StringPtr specifier, bool internal, kj::OneOf<CppModuleContents,
                TypeScriptModuleContents> contents)
        : specifier(specifier),
          internal(internal),
          contents(kj::mv(contents)) {}

    kj::StringPtr specifier;
    bool internal;
    kj::OneOf<CppModuleContents, TypeScriptModuleContents> contents;
  };

  void addBuiltinBundle(jsg::Bundle::Reader bundle) {
    for (auto module: bundle.getModules()) {
      TypeScriptModuleContents contents (module.getTsDeclaration());
      ModuleInfo info (module.getName(), module.getInternal(), kj::mv(contents));
      modules.add(kj::mv(info));
    }
  }

  template <typename T>
  void addBuiltinModule(kj::StringPtr specifier, jsg::ModuleRegistry::Type type = jsg::ModuleRegistry::Type::BUILTIN) {
    auto internal = type == jsg::ModuleRegistry::Type::INTERNAL;
    auto structureName = jsg::fullyQualifiedTypeName(typeid(T));
    CppModuleContents contents (kj::mv(structureName));
    ModuleInfo info (specifier, internal, kj::mv(contents));
    modules.add(kj::mv(info));
  }

  kj::Vector<ModuleInfo> modules;
};

CompatibilityFlags::Reader compileFlags(capnp::MessageBuilder &message, kj::StringPtr compatDate,
                                        bool experimental, kj::ArrayPtr<kj::String> compatFlags) {
  // Based on src/workerd/io/compatibility-date-test.c++
  auto orphanage = message.getOrphanage();
  auto flagListOrphan =
      orphanage.newOrphan<capnp::List<capnp::Text>>(compatFlags.size());
  auto flagList = flagListOrphan.get();
  for (auto i : kj::indices(compatFlags)) {
    flagList.set(i, compatFlags.begin()[i]);
  }

  auto output = message.initRoot<CompatibilityFlags>();
  EncoderErrorReporterImpl errorReporter;

  compileCompatibilityFlags(compatDate, flagList.asReader(), output,
                            errorReporter, experimental,
                            CompatibilityDateValidation::FUTURE_FOR_TEST);

  if (!errorReporter.errors.empty()) {
    // TODO(someday): throw an `AggregateError` containing all errors
    JSG_FAIL_REQUIRE(Error, errorReporter.errors[0]);
  }

  auto reader = output.asReader();
  return kj::mv(reader);
}

struct TypesEncoder {
public:
  TypesEncoder(kj::String compatDate, kj::Array<kj::String> compatFlags): compatDate(kj::mv(compatDate)), compatFlags(kj::mv(compatFlags)) {}

  kj::Array<byte> encode() {
    capnp::MallocMessageBuilder flagsMessage;
    CompatibilityFlags::Reader flags = compileFlags(flagsMessage, compatDate, false, compatFlags);

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
      auto& module = registry.modules[i++];
      moduleBuilder.setSpecifier(module.specifier);
      moduleBuilder.setInternal(module.internal);
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
  void writeGroup(
      capnp::List<jsg::rtti::StructureGroups::StructureGroup>::Builder &groups,
      jsg::rtti::Builder<CompatibilityFlags::Reader> &builder, kj::StringPtr name) {
    auto group = groups[groupsIndex++];
    group.setName(name);

    unsigned int structuresSize = sizeof...(Types);
    auto structures = group.initStructures(structuresSize);
    structureIndex = 0;
    (writeStructure<Types>(builder, structures), ...);
    KJ_ASSERT(structureIndex == structuresSize);
  }

  kj::String compatDate;
  kj::Array<kj::String> compatFlags;

  unsigned int groupsIndex = 0;
  unsigned int structureIndex = 0;
};

} // namespace

kj::Array<byte> RTTIModule::exportTypes(kj::String compatDate, kj::Array<kj::String> compatFlags) {
  TypesEncoder encoder(kj::mv(compatDate), kj::mv(compatFlags));
  return encoder.encode();
}

} // namespace workerd::api
