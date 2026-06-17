#include "frankenvalue.h"

#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>

namespace workerd {

Frankenvalue Frankenvalue::cloneImpl() const {
  Frankenvalue result;

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(_, EmptyObject) {
      result.value = EmptyObject();
    }
    KJ_CASE_ONEOF(json, Json) {
      result.value = Json{kj::str(json.json)};
    }
    KJ_CASE_ONEOF(v8Serialized, V8Serialized) {
      result.value = V8Serialized{kj::heapArray(v8Serialized.data.asPtr())};
    }
    KJ_CASE_ONEOF(capability, Capability) {
      result.value = capability;
    }
  }

  if (!properties.empty()) {
    result.properties.reserve(properties.size());

    for (auto& property: properties) {
      KJ_ASSERT(property.value.capTable.empty());
      result.properties.add(Property{
        .name = kj::str(property.name),
        .value = property.value.cloneImpl(),
        .capTableOffset = property.capTableOffset,
        .capTableSize = property.capTableSize,
      });
    }
  }

  return result;
}

Frankenvalue Frankenvalue::clone() {
  auto result = cloneImpl();

  if (!capTable.empty()) {
    result.capTable.reserve(capTable.size());
    for (auto& entry: capTable) {
      result.capTable.add(entry->clone());
    }
  }

  return result;
}

Frankenvalue Frankenvalue::threadSafeClone() const {
  auto result = cloneImpl();

  if (!capTable.empty()) {
    result.capTable.reserve(capTable.size());
    for (auto& entry: capTable) {
      result.capTable.add(entry->threadSafeClone());
    }
  }

  return result;
}

kj::Own<Frankenvalue::CapTableEntry> Frankenvalue::CapTableEntry::threadSafeClone() const {
  KJ_FAIL_REQUIRE("a capability in the Frankenvalue does not implement threadSafeClone()");
}

void Frankenvalue::toCapnp(rpc::Frankenvalue::Builder builder) {
  toCapnpImpl(builder, capTable.size());
}

void Frankenvalue::toCapnpImpl(rpc::Frankenvalue::Builder builder, size_t capTableSize) {
  KJ_REQUIRE(capTableSize <= static_cast<uint32_t>(kj::maxValue),
      "Frankenvalue capTable is too large to serialize");

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(_, EmptyObject) {
      builder.setEmptyObject();
    }
    KJ_CASE_ONEOF(json, Json) {
      builder.setJson(json.json);
    }
    KJ_CASE_ONEOF(v8Serialized, V8Serialized) {
      builder.setV8Serialized(v8Serialized.data);
    }
    KJ_CASE_ONEOF(capability, Capability) {
      // Defense-in-depth: the cap index must reference one of this node's base caps. fromCapnp()
      // enforces the same invariant on the decode side (see fromCapnpImpl()).
      KJ_REQUIRE(capability.capIndex < capTableSize, "Frankenvalue capability index out of range",
          capability.capIndex, capTableSize);
      auto capBuilder = builder.initCapability();
      capBuilder.setCapIndex(capability.capIndex);
      capBuilder.setTag(capability.tag);
    }
  }

  if (properties.empty()) {
    builder.setCapTableSize(static_cast<uint32_t>(capTableSize));
  } else {
    size_t capTablePos = properties[0].capTableOffset;
    builder.setCapTableSize(static_cast<uint32_t>(capTablePos));

    auto listBuilder = builder.initProperties(properties.size());

    for (auto i: kj::indices(properties)) {
      KJ_ASSERT(properties[i].capTableOffset == capTablePos);
      capTablePos += properties[i].capTableSize;
      auto elemBuilder = listBuilder[i];
      elemBuilder.setName(properties[i].name);
      properties[i].value.toCapnpImpl(elemBuilder, properties[i].capTableSize);
    }
    KJ_ASSERT(capTablePos == capTableSize);
  }
}

Frankenvalue Frankenvalue::fromCapnp(
    rpc::Frankenvalue::Reader reader, kj::Vector<kj::Own<CapTableEntry>> capTable) {
  Frankenvalue result;

  size_t capCount = result.fromCapnpImpl(reader, 0, capTable.size());

  KJ_REQUIRE(capTable.size() == capCount, "Frankenvalue capTable size doesn't match contents");
  result.capTable = kj::mv(capTable);

  return result;
}

size_t Frankenvalue::fromCapnpImpl(
    rpc::Frankenvalue::Reader reader, size_t capCount, size_t capTableTotal) {
  switch (reader.which()) {
    case rpc::Frankenvalue::EMPTY_OBJECT:
      this->value = EmptyObject();
      break;
    case rpc::Frankenvalue::JSON:
      this->value = Json{kj::str(reader.getJson())};
      break;
    case rpc::Frankenvalue::V8_SERIALIZED:
      this->value = V8Serialized{kj::heapArray(reader.getV8Serialized())};
      break;
    case rpc::Frankenvalue::CAPABILITY: {
      auto cap = reader.getCapability();
      // The tag is untrusted, but we can't validate it here: resolving it to a deserializer
      // requires the isolate's serialization registry (and a jsg::Lock), neither of which is
      // available at fromCapnp() time. toJs() consults the registry and throws ("no deserializer
      // registered for Frankenvalue capability tag") if the tag is unknown, so an invalid tag is
      // rejected before any capability is materialized.
      this->value = Capability{.capIndex = cap.getCapIndex(), .tag = cap.getTag()};
      break;
    }
  }

  size_t nodeCaps = reader.getCapTableSize();
  // Security invariant: never create OOB cap table slices.
  KJ_REQUIRE(nodeCaps <= capTableTotal - capCount, "Frankenvalue capTableSize exceeds capTable");

  // A `capability` value references one of this node's base caps by index; make sure it's in
  // range so that toJs() can't read out of bounds. A capability node must own at least one base
  // cap: capIndex is unsigned, so `capIndex < nodeCaps` already rejects nodeCaps == 0, but we
  // assert it explicitly to document the invariant.
  KJ_IF_SOME(capability, this->value.tryGet<Capability>()) {
    KJ_REQUIRE(nodeCaps >= 1, "Frankenvalue capability node has no caps");
    KJ_REQUIRE(capability.capIndex < nodeCaps, "Frankenvalue capability index out of range");
  }

  capCount += nodeCaps;

  auto properties = reader.getProperties();
  if (properties.size() > 0) {
    this->properties.reserve(properties.size());

    for (auto property: properties) {
      Property result{
        .name = kj::str(property.getName()),
        .capTableOffset = capCount,
      };
      capCount = result.value.fromCapnpImpl(property, capCount, capTableTotal);
      result.capTableSize = capCount - result.capTableOffset;
      this->properties.add(kj::mv(result));
    }
  }

  return capCount;
}

jsg::JsValue Frankenvalue::toJs(jsg::Lock& js) {
  return toJsImpl(js, capTable);
}

jsg::JsValue Frankenvalue::toJsImpl(jsg::Lock& js, kj::ArrayPtr<kj::Own<CapTableEntry>> capTable) {
  return js.withinHandleScope([&]() -> jsg::JsValue {
    jsg::JsValue result = [&]() -> jsg::JsValue {
      KJ_SWITCH_ONEOF(value) {
        KJ_CASE_ONEOF(_, EmptyObject) {
          return js.obj();
        }
        KJ_CASE_ONEOF(json, Json) {
          // TODO(cleanup): Make jsg::Lock::parseJson() not return a persistent handle.
          return jsg::JsValue(jsg::check(v8::JSON::Parse(js.v8Context(), js.str(json.json))));
        }
        KJ_CASE_ONEOF(v8Serialized, V8Serialized) {
          CapTableReader capTableReader(
              properties.empty() ? capTable : capTable.first(properties[0].capTableOffset));

          jsg::Deserializer deser(js, v8Serialized.data, kj::none, kj::none,
              jsg::Deserializer::Options{
                .externalHandler = capTableReader,
              });
          return deser.readValue(js);
        }
        KJ_CASE_ONEOF(capability, Capability) {
          // The value is a single capability taken directly from the cap table, without going
          // through V8 serialization. We materialize it by invoking the deserializer registered
          // for the capability's `tag` (e.g. `Fetcher::deserialize` for `serviceStub`), feeding it
          // a `Deserializer` whose external handler is our cap table and whose first raw value is
          // the cap index -- exactly what those deserializers expect to read.
          CapTableReader capTableReader(
              properties.empty() ? capTable : capTable.first(properties[0].capTableOffset));

          // TODO(perf): This round-trips through a Serializer/Deserializer (one heap allocation)
          //   solely to hand the registered deserializer a `Deserializer` from which it reads a
          //   single uint32 cap index. It's a cold path (once per capability at binding setup), so
          //   not worth optimizing now, but a direct API to invoke the deserializer with the index
          //   would avoid the allocation.
          jsg::Serializer payloadSer(js);
          payloadSer.writeRawUint32(capability.capIndex);
          auto payload = payloadSer.release();

          jsg::Deserializer deser(js, payload.data, kj::none, kj::none,
              jsg::Deserializer::Options{
                .externalHandler = capTableReader,
              });

          auto obj = jsg::IsolateBase::from(js.v8Isolate).deserialize(js, capability.tag, deser);
          return jsg::JsValue(v8::Local<v8::Value>(KJ_REQUIRE_NONNULL(
              obj, "no deserializer registered for Frankenvalue capability tag", capability.tag)));
        }
      }
      KJ_UNREACHABLE;
    }();

    if (!properties.empty()) {
      jsg::JsObject obj = KJ_REQUIRE_NONNULL(
          result.tryCast<jsg::JsObject>(), "non-object Frankenvalue can't have properties");

      for (auto& property: properties) {
        obj.set(js, property.name,
            property.value.toJsImpl(js,
                capTable.slice(
                    property.capTableOffset, property.capTableOffset + property.capTableSize)));
      }
    }

    return result;
  });
}

void Frankenvalue::populateJsObject(jsg::Lock& js, jsg::JsObject target) {
  if (!empty()) {
    js.withinHandleScope([&]() {
      auto sourceObj = KJ_REQUIRE_NONNULL(toJs(js).tryCast<jsg::JsObject>(),
          "Frankenvalue must be an object for populateJsObject()");
      auto props = sourceObj.getPropertyNames(js, jsg::KeyCollectionFilter::OWN_ONLY,
          jsg::PropertyFilter::ONLY_ENUMERABLE, jsg::IndexFilter::INCLUDE_INDICES);
      auto propCount = props.size();
      for (auto i: kj::zeroTo(propCount)) {
        auto prop = props.get(js, i);
        target.set(js, prop, sourceObj.get(js, prop));
      }
    });
  }
}

Frankenvalue Frankenvalue::fromJs(jsg::Lock& js, jsg::JsValue value) {
  Frankenvalue result;

  js.withinHandleScope([&]() {
    CapTableBuilder capTableBuilder(result);
    jsg::Serializer ser(js,
        {
          .treatClassInstancesAsPlainObjects = false,
          .externalHandler = capTableBuilder,
        });
    ser.write(js, value);
    result.value = V8Serialized{ser.release().data};
  });

  return result;
}

Frankenvalue Frankenvalue::fromJson(kj::String json) {
  Frankenvalue result;
  result.value = Json{kj::mv(json)};
  return result;
}

size_t Frankenvalue::estimateSize() const {
  size_t result = 0;

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(_, EmptyObject) {}
    KJ_CASE_ONEOF(json, Json) {
      result += json.json.size();
    }
    KJ_CASE_ONEOF(v8Serialized, V8Serialized) {
      result += v8Serialized.data.size();
    }
    KJ_CASE_ONEOF(capability, Capability) {
      result += sizeof(Capability);
    }
  }

  for (auto& property: properties) {
    result += property.name.size();
    result += property.value.estimateSize();
  }

  return result;
}

Frankenvalue Frankenvalue::fromCapability(uint16_t tag, kj::Own<CapTableEntry> entry) {
  Frankenvalue result;
  result.value = Capability{.capIndex = 0, .tag = tag};
  result.capTable.add(kj::mv(entry));
  return result;
}

void Frankenvalue::setProperty(kj::String name, Frankenvalue value) {
  // We need to merge the value's cap table into ours.
  uint capTableOffset = capTable.size();
  uint capTableSize = value.capTable.size();

  capTable.reserve(capTable.size() + value.capTable.size());
  for (auto& cap: value.capTable) {
    capTable.add(kj::mv(cap));
  }

  // Drop the value's capTable. Overwrite it rather than use clear() so that the backing buffer
  // is actually freed.
  value.capTable = {};

  properties.add(Property{
    .name = kj::mv(name),
    .value = kj::mv(value),
    .capTableOffset = capTableOffset,
    .capTableSize = capTableSize,
  });
}

FrankenvalueHandler& getUnsupportedFrankenvalueHandler() {
  class UnsupportedFrankenvalueHandler final: public FrankenvalueHandler {
   public:
    void toCapnp(Frankenvalue& value, rpc::Frankenvalue::Builder builder) override {
      KJ_FAIL_REQUIRE("Frankenvalues unsupported here");
    }
  };
  static UnsupportedFrankenvalueHandler instance;
  return instance;
}

}  // namespace workerd
