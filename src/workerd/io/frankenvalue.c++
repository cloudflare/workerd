#include "frankenvalue.h"

#include <workerd/jsg/ser.h>

namespace workerd {

Frankenvalue Frankenvalue::clone() const {
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

  if (properties.size() > 0) {
    result.properties.reserve(properties.size());

    for (auto& property: properties) {
      result.properties.add(Property{
        .name = kj::str(property.name),
        .value = property.value.clone(),
      });
    }
  }

  return result;
}

void Frankenvalue::toCapnp(rpc::Frankenvalue::Builder builder) const {
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

  if (properties.size() > 0) {
    auto listBuilder = builder.initProperties(properties.size());

    for (auto i: kj::indices(properties)) {
      auto elemBuilder = listBuilder[i];
      elemBuilder.setName(properties[i].name);
      properties[i].value.toCapnp(elemBuilder);
    }
  }
}

Frankenvalue Frankenvalue::fromCapnp(rpc::Frankenvalue::Reader reader) {
  Frankenvalue result;

  switch (reader.which()) {
    case rpc::Frankenvalue::EMPTY_OBJECT:
      result.value = EmptyObject();
      break;
    case rpc::Frankenvalue::JSON:
      result.value = Json{kj::str(reader.getJson())};
      break;
    case rpc::Frankenvalue::V8_SERIALIZED:
      result.value = V8Serialized{kj::heapArray(reader.getV8Serialized())};
      break;
  }

  auto properties = reader.getProperties();
  if (properties.size() > 0) {
    result.properties.reserve(properties.size());

    for (auto property: properties) {
      result.properties.add(Property{
        .name = kj::str(property.getName()),
        .value = fromCapnp(property),
      });
    }
  }

  return result;
}

jsg::JsValue Frankenvalue::toJs(jsg::Lock& js) const {
  // TODO(cleanup): Make `withinHandleScope()` correctly support `jsg::JsValue` and friends.
  return jsg::JsValue(js.withinHandleScope([&]() -> v8::Local<v8::Value> {
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
          jsg::Deserializer deser(js, v8Serialized.data);
          return deser.readValue(js);
        }
      }
      KJ_UNREACHABLE;
    }();

    if (properties.size() > 0) {
      jsg::JsObject obj = KJ_REQUIRE_NONNULL(
          result.tryCast<jsg::JsObject>(), "non-object Frakenvalue can't have properties");

      for (auto& property: properties) {
        obj.set(js, property.name, property.value.toJs(js));
      }
    }

    return result;
  }));
}

Frankenvalue Frankenvalue::fromJs(jsg::Lock& js, jsg::JsValue value) {
  Frankenvalue result;

  js.withinHandleScope([&]() {
    jsg::Serializer ser(js, {.treatClassInstancesAsPlainObjects = false});
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
  properties.add(Property{
    .name = kj::mv(name),
    .value = kj::mv(value),
  });
}

}  // namespace workerd
