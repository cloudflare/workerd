#include "frankenvalue.h"

#include <workerd/jsg/ser.h>

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
  }

  size_t nodeCaps = reader.getCapTableSize();
  // Security invariant: never create OOB cap table slices.
  KJ_REQUIRE(nodeCaps <= capTableTotal - capCount, "Frankenvalue capTableSize exceeds capTable");
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

}  // namespace workerd
