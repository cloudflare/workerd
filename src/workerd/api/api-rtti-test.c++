// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/queue.h>
#include <workerd/api/rtti.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/sync-kv.h>
#include <workerd/api/trace.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/urlpattern-standard.h>
#include <workerd/api/urlpattern.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/rtti.h>

#include <capnp/serialize.h>
#include <kj/test.h>

// Test building rtti for various APIs.

namespace workerd::api {
namespace {

KJ_TEST("WorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<WorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::Event"_kj) != kj::none);
  KJ_EXPECT(builder.structure("workerd::api::ObviouslyWrongName"_kj) == kj::none);
}

KJ_TEST("ServiceWorkerGlobalScope") {
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  builder.structure<ServiceWorkerGlobalScope>();
  KJ_EXPECT(builder.structure("workerd::api::DurableObjectId"_kj) != kj::none);
}

KJ_TEST("JsReadableStream delegates its RTTI to ReadableStream") {
  // JsReadableStream declares `using JsgRttiDelegate = jsg::Ref<ReadableStream>`, so RTTI (and
  // therefore generated TypeScript) must describe it exactly as it describes ReadableStream.
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  auto type = builder.type<JsReadableStream>();
  KJ_ASSERT(type.isStructure());
  KJ_EXPECT(type.getStructure().getFullyQualifiedName() == "workerd::api::ReadableStream"_kj);
  KJ_EXPECT(builder.structure("workerd::api::ReadableStream"_kj) != kj::none);
}

KJ_TEST("JsWritableStream delegates its RTTI to WritableStream") {
  // JsWritableStream declares `using JsgRttiDelegate = jsg::Ref<WritableStream>`, so RTTI (and
  // therefore generated TypeScript) must describe it exactly as it describes WritableStream.
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  auto type = builder.type<JsWritableStream>();
  KJ_ASSERT(type.isStructure());
  KJ_EXPECT(type.getStructure().getFullyQualifiedName() == "workerd::api::WritableStream"_kj);
  KJ_EXPECT(builder.structure("workerd::api::WritableStream"_kj) != kj::none);
}

KJ_TEST("JsReadableWritablePair delegates its RTTI to ReadableStream::Transform") {
  // JsReadableWritablePair declares `using JsgRttiDelegate = ReadableStream::Transform`, so RTTI
  // (and therefore generated TypeScript) must describe it exactly as it describes the
  // ReadableWritablePair-shaped Transform struct.
  jsg::rtti::Builder builder((CompatibilityFlags::Reader()));
  auto type = builder.type<JsReadableWritablePair>();
  KJ_ASSERT(type.isStructure());
  KJ_EXPECT(
      type.getStructure().getFullyQualifiedName() == "workerd::api::ReadableStream::Transform"_kj,
      type.getStructure().getFullyQualifiedName());
}

KJ_TEST("RTTI export discovers Rust modules and their referenced structs without evaluating JS") {
  auto module = jsg::alloc<RTTIModule>();
  auto bytes = module->exportTypes(kj::str("2026-01-01"), kj::heapArray<kj::String>(0));
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);
  capnp::FlatArrayMessageReader reader(words);
  auto root = reader.getRoot<jsg::rtti::StructureGroups>();

  kj::Maybe<kj::StringPtr> dnsIdentity;
  kj::Maybe<kj::StringPtr> recordIdentity;
  for (auto group: root.getGroups()) {
    for (auto structure: group.getStructures()) {
      if (structure.getTsName() == "DnsUtil") {
        dnsIdentity = structure.getFullyQualifiedName();
        KJ_EXPECT(!structure.getTsRoot());
        for (auto member: structure.getMembers()) {
          if (member.isMethod() && member.getMethod().getName() == "parseCaaRecord") {
            auto method = member.getMethod();
            KJ_ASSERT(method.getArgs().size() == 1);
            KJ_EXPECT(method.getArgs()[0].isString());
            KJ_EXPECT(method.getArgNames()[0] == "record");
            KJ_ASSERT(method.getReturnType().isStructure());
            recordIdentity = method.getReturnType().getStructure().getFullyQualifiedName();
          }
        }
      }
    }
  }
  KJ_ASSERT(dnsIdentity != kj::none);
  KJ_ASSERT(recordIdentity != kj::none);
  bool foundRecord = false;
  bool foundModule = false;
  for (auto group: root.getGroups()) {
    for (auto structure: group.getStructures()) {
      if (structure.getFullyQualifiedName() == KJ_ASSERT_NONNULL(recordIdentity)) {
        foundRecord = true;
        KJ_EXPECT(structure.getTsName() == "CaaRecord");
        for (auto field: kj::arr("critical"_kj, "field"_kj, "value"_kj)) {
          bool foundField = false;
          for (auto member: structure.getMembers()) {
            if (member.isProperty() && member.getProperty().getName() == field) {
              foundField = true;
            }
          }
          KJ_EXPECT(foundField, field);
        }
        KJ_EXPECT(!structure.getTsRoot());
      }
    }
  }
  for (auto entry: root.getModules()) {
    if (entry.getSpecifier() == "node-internal:dns") {
      KJ_EXPECT(entry.getStructureName() == KJ_ASSERT_NONNULL(dnsIdentity));
      foundModule = true;
    }
  }
  KJ_EXPECT(foundRecord);
  KJ_EXPECT(foundModule);
}

}  // namespace
}  // namespace workerd::api
