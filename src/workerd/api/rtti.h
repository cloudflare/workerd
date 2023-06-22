// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <initializer_list>
#include <kj/array.h>
#include <kj/string.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/rtti.h>

namespace workerd::api {

struct TypesEncoder {
public:
  TypesEncoder(kj::String compatDate, kj::Array<kj::String> compatFlags): compatDate(kj::mv(compatDate)), compatFlags(kj::mv(compatFlags)) {}

  kj::Array<byte> encode();

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

class RTTIModule final: public jsg::Object {
public:
  kj::Array<byte> exportTypes(kj::String compatDate, kj::Array<kj::String> compatFlags) {
    TypesEncoder encoder(kj::mv(compatDate), kj::mv(compatFlags));
    return encoder.encode();
  }

  JSG_RESOURCE_TYPE(RTTIModule) {
    JSG_METHOD(exportTypes);
  }
};

template <class Registry>
void registerRTTIModule(Registry& registry) {
  registry.template addBuiltinModule<RTTIModule>("workerd:rtti",
    workerd::jsg::ModuleRegistry::Type::BUILTIN);
}

#define EW_RTTI_ISOLATE_TYPES api::RTTIModule

}
