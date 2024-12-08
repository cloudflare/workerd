// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

class Frankenvalue {
 public:
  Frankenvalue(): value(EmptyObject()) {}

  bool empty() const {
    return value.is<EmptyObject>() && properties.empty();
  }

  Frankenvalue clone() const;

  // Convert to/from capnp format.
  void toCapnp(rpc::Frankenvalue::Builder builder) const;
  static Frankenvalue fromCapnp(rpc::Frankenvalue::Reader reader);

  // Convert to/from JavaScript values. Note that round trips here don't produce the exact same
  // Frankenvalue representation: toJs() puts all the contents together into a single value, and
  // fromJs() always returns a Frakenvalue containing a single V8-serialized value.
  jsg::JsValue toJs(jsg::Lock& js) const;
  static Frankenvalue fromJs(jsg::Lock& js, jsg::JsValue value);

  // Construct a Frakenvalue from JSON.
  //
  // (It's not possible to convert a Frakenvalue back to JSON, except by evaluating it in JS and
  // then JSON-stringifying from there.)
  static Frankenvalue fromJson(kj::String json);

  // Add a property to the value, represented as another Frankenvalue. This is how you "stitch
  // together" values!
  //
  // This is called `set` because the new property will override any existing property with the
  // same name, but note that this strictly appends content. The replacement happens only when the
  // Frankenvalue is finally converted to JS.
  void setProperty(kj::String name, Frankenvalue value);

 private:
  struct EmptyObject {};
  struct Json {
    kj::String json;
  };
  struct V8Serialized {
    kj::Array<byte> data;
  };
  kj::OneOf<EmptyObject, Json, V8Serialized> value;

  struct Property;
  kj::Vector<Property> properties;
};

// Can't be defined inline since `Frankenvalue` is still incomplete there.
struct Frankenvalue::Property {
  kj::String name;
  Frankenvalue value;
};

}  // namespace workerd
