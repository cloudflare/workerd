// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Encodes JSG RTTI for all APIs defined in `src/workerd/api` to a capnp binary
// for consumption by other tools (e.g. TypeScript type generation).

#include <capnp/serialize-packed.h>
#include <initializer_list>
#include <kj/filesystem.h>
#include <kj/main.h>
#include <workerd/api/actor.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/kv.h>
#include <workerd/api/r2.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/node/node.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/rtti.h>

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
  F("node", EW_NODE_ISOLATE_TYPES)

namespace workerd::api {
namespace {

using namespace jsg;

struct ApiEncoderErrorReporterImpl : public Worker::ValidationErrorReporter {
  void addError(kj::String error) override { errors.add(kj::mv(error)); }
  void addHandler(kj::Maybe<kj::StringPtr> exportName,
                  kj::StringPtr type) override {
    KJ_UNREACHABLE;
  }

  kj::Vector<kj::String> errors;
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

  CompatibilityFlags::Reader
  compileFlags(capnp::MessageBuilder &message, kj::StringPtr compatDate, bool experimental,
               kj::ArrayPtr<const kj::StringPtr> compatFlags) {
    // Based on src/workerd/io/compatibility-date-test.c++
    auto orphanage = message.getOrphanage();
    auto flagListOrphan =
        orphanage.newOrphan<capnp::List<capnp::Text>>(compatFlags.size());
    auto flagList = flagListOrphan.get();
    for (auto i : kj::indices(compatFlags)) {
      flagList.set(i, compatFlags.begin()[i]);
    }

    auto output = message.initRoot<CompatibilityFlags>();
    ApiEncoderErrorReporterImpl errorReporter;

    compileCompatibilityFlags(compatDate, flagList.asReader(), output,
                              errorReporter, experimental,
                              CompatibilityDateValidation::FUTURE_FOR_TEST);

    if (!errorReporter.errors.empty()) {
      KJ_FAIL_ASSERT(kj::strArray(errorReporter.errors, "\n"));
    }

    auto reader = output.asReader();
    return kj::mv(reader);
  }

  bool run() {
    // Create RTTI builder with all non-experimental compatibility flags enabled
    capnp::MallocMessageBuilder flagsMessage;
    CompatibilityFlags::Reader flags;
    KJ_IF_MAYBE (date, compatibilityDate) {
      flags = compileFlags(flagsMessage, *date, false, {});
    } else {
      flags = compileFlags(flagsMessage, "2021-01-01", false, {});
    }
    auto builder = rtti::Builder(flags);

    // Build structure groups
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<rtti::StructureGroups>();

#define EW_TYPE_GROUP_COUNT(Name, Types) groupsSize++;
#define EW_TYPE_GROUP_WRITE(Name, Types)                                       \
  writeGroup<Types>(groups, builder, Name);

    unsigned int groupsSize = 0;
    EW_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_COUNT)
    auto groups = root.initGroups(groupsSize);
    groupsIndex = 0;
    EW_TYPE_GROUP_FOR_EACH(EW_TYPE_GROUP_WRITE)
    KJ_ASSERT(groupsIndex == groupsSize);

#undef EW_TYPE_GROUP_COUNT
#undef EW_TYPE_GROUP_WRITE

    // Write structure groups to a file or stdout if none specifed
    KJ_IF_MAYBE (value, output) {
      auto fs = kj::newDiskFilesystem();
      auto path = kj::Path::parse(*value);
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
