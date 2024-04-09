// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Encodes JSG RTTI for all APIs defined in `src/workerd/api` to a capnp binary
// for consumption by other tools (e.g. TypeScript type generation).

// When creating type definitions, only include the API headers to reduce the clang AST dump size.
#if !API_ENCODER_HDRS_ONLY
#include <capnp/serialize-packed.h>
#include <initializer_list>
#include <kj/filesystem.h>
#include <kj/main.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/rtti.h>
#endif // !API_ENCODER_HDRS_ONLY

#include <workerd/api/index.h>

#if !API_ENCODER_HDRS_ONLY

#define EW_API_ENCODER_TYPE_GROUP_FOR_EACH(F)                                  \
  F("dom-exception", jsg::DOMException)                                        \
  EW_TYPE_GROUP_FOR_EACH(F)

namespace workerd::api {
namespace {

using namespace jsg;

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
    ModuleInfo(kj::StringPtr specifier, jsg::ModuleType type, kj::OneOf<CppModuleContents,
                TypeScriptModuleContents> contents)
        : specifier(specifier),
          type(type),
          contents(kj::mv(contents)) {}

    kj::StringPtr specifier;
    jsg::ModuleType type;
    kj::OneOf<CppModuleContents, TypeScriptModuleContents> contents;
  };

  void addBuiltinBundle(jsg::Bundle::Reader bundle, kj::Maybe<jsg::ModuleRegistry::Type> maybeFilter = kj::none) {
    for (auto module: bundle.getModules()) {
      if (module.getType() == maybeFilter.orDefault(module.getType())) addBuiltinModule(module);
    }
  }

  void addBuiltinModule(jsg::Module::Reader module) {
    TypeScriptModuleContents contents (module.getTsDeclaration());
    ModuleInfo info (module.getName(), module.getType(), kj::mv(contents));
    modules.add(kj::mv(info));
  }

  template <typename T>
  void addBuiltinModule(kj::StringPtr specifier, jsg::ModuleRegistry::Type type = jsg::ModuleRegistry::Type::BUILTIN) {
    auto structureName = jsg::fullyQualifiedTypeName(typeid(T));
    CppModuleContents contents (kj::mv(structureName));
    ModuleInfo info (specifier, type, kj::mv(contents));
    modules.add(kj::mv(info));
  }

  kj::Vector<ModuleInfo> modules;
};

struct ApiEncoderMain {
  explicit ApiEncoderMain(kj::ProcessContext &context) : context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "<unknown>", "API Encoder")
        .addOptionWithArg({"o", "output"}, KJ_BIND_METHOD(*this, setOutput),
                          "<file>", "Output to <file>")
        .addOptionWithArg(
            {"c", "compatibility-date"},
            KJ_BIND_METHOD(*this, setCompatibilityDate), "<date>",
            "Set the compatibility date of the generated types to <date>")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  kj::MainBuilder::Validity setOutput(kj::StringPtr value) {
    output = value;
    return true;
  }

  kj::MainBuilder::Validity setCompatibilityDate(kj::StringPtr value) {
    compatibilityDate = value;
    return true;
  }

  CompatibilityFlags::Reader compileFlags(capnp::MessageBuilder &message, kj::StringPtr compatDate,
                                          kj::ArrayPtr<const kj::StringPtr> compatFlags) {
    // Based on src/workerd/io/compatibility-date-test.c++
    auto orphanage = message.getOrphanage();
    auto flagListOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(compatFlags.size());
    auto flagList = flagListOrphan.get();
    for (auto i : kj::indices(compatFlags)) {
      flagList.set(i, compatFlags.begin()[i]);
    }

    auto output = message.initRoot<CompatibilityFlags>();
    SimpleWorkerErrorReporter errorReporter;
    compileCompatibilityFlags(compatDate, flagList.asReader(), output, errorReporter,
                              /* experimental */ true,
                              CompatibilityDateValidation::FUTURE_FOR_TEST);

    if (!errorReporter.errors.empty()) {
      KJ_FAIL_ASSERT(kj::strArray(errorReporter.errors, "\n"));
    }

    auto reader = output.asReader();
    return kj::mv(reader);
  }

  CompatibilityFlags::Reader compileAllFlags(capnp::MessageBuilder &message) {
    auto output = message.initRoot<CompatibilityFlags>();
    auto schema = capnp::Schema::from<CompatibilityFlags>();
    auto dynamicOutput = capnp::toDynamic(output);
    for (auto field: schema.getFields()) dynamicOutput.set(field, true);
    auto reader = output.asReader();
    return kj::mv(reader);
  }

  bool run() {
    capnp::MallocMessageBuilder flagsMessage;
    CompatibilityFlags::Reader flags;
    KJ_IF_SOME (date, compatibilityDate) {
      flags = compileFlags(flagsMessage, date, {});
    } else {
      flags = compileAllFlags(flagsMessage);
    }
    auto builder = rtti::Builder(flags);

    // Build structure groups
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<rtti::StructureGroups>();

#define EW_TYPE_GROUP_COUNT(Name, Types) groupsSize++;
#define EW_TYPE_GROUP_WRITE(Name, Types) writeGroup<Types>(groups, builder, Name);

    unsigned int groupsSize = 0;
    EW_API_ENCODER_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_COUNT)
    auto groups = root.initGroups(groupsSize);
    groupsIndex = 0;
    EW_API_ENCODER_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_WRITE)
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
      KJ_SWITCH_ONEOF(module.contents) {
        KJ_CASE_ONEOF(contents, EncoderModuleRegistryImpl::CppModuleContents) {
          moduleBuilder.setStructureName(contents.structureName);
        }
        KJ_CASE_ONEOF(contents, EncoderModuleRegistryImpl::TypeScriptModuleContents) {
          moduleBuilder.setTsDeclarations(contents.tsDeclarations);
        }
      }
    }

    // Write structure groups to a file or stdout if none specifed
    KJ_IF_SOME (value, output) {
      auto fs = kj::newDiskFilesystem();
      auto path = kj::Path::parse(value);
      auto writeMode = kj::WriteMode::CREATE | kj::WriteMode::MODIFY |
                       kj::WriteMode::CREATE_PARENT;
      auto file = fs->getCurrent().openFile(path, writeMode);
      auto words = capnp::messageToFlatArray(message);
      auto bytes = words.asBytes();
      file->writeAll(bytes);
    } else {
      capnp::writeMessageToFd(1 /* stdout */, message);
    }

    return true;
  }

  template <typename Type>
  void writeStructure(rtti::Builder<CompatibilityFlags::Reader> &builder,
                      capnp::List<rtti::Structure>::Builder structures) {
    auto reader = builder.structure<Type>();
    structures.setWithCaveats(structureIndex++, reader);
  }

  template <typename... Types>
  void writeGroup(
      capnp::List<rtti::StructureGroups::StructureGroup>::Builder &groups,
      rtti::Builder<CompatibilityFlags::Reader> &builder, kj::StringPtr name) {
    auto group = groups[groupsIndex++];
    group.setName(name);

    unsigned int structuresSize = sizeof...(Types);
    auto structures = group.initStructures(structuresSize);
    structureIndex = 0;
    (writeStructure<Types>(builder, structures), ...);
    KJ_ASSERT(structureIndex == structuresSize);
  }

private:
  kj::ProcessContext &context;
  kj::Maybe<kj::StringPtr> output;
  kj::Maybe<kj::StringPtr> compatibilityDate;

  unsigned int groupsIndex = 0;
  unsigned int structureIndex = 0;
};

} // namespace
} // namespace workerd::api

KJ_MAIN(workerd::api::ApiEncoderMain);

#endif // !API_ENCODER_HDRS_ONLY

