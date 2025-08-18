// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

// C++ class mirroring `Frankenvalue` as defined in `frankenvalue.capnp`.
//
// Represents a JavaScript value that has been stitched together from multiple sources outside of
// a JavaScript evaluation context. The Frankevalue can be evaluated down to a JS value as soon
// as it has a JS execution environment in which to be evaluated.
//
// This is used in particular to represent `ctx.props`.
class Frankenvalue {
 public:
  Frankenvalue(): value(EmptyObject()) {}

  bool empty() const {
    return value.is<EmptyObject>() && properties.empty();
  }

  Frankenvalue clone();

  class CapTableEntry;

  // Convert to/from capnp format.
  //
  // The CapTable, if any, is expected to be handled separately, as different use cases call for
  // very different handling of the cap table.
  void toCapnp(rpc::Frankenvalue::Builder builder);
  static Frankenvalue fromCapnp(
      rpc::Frankenvalue::Reader reader, kj::Vector<kj::Own<CapTableEntry>> capTable = {});

  // Convert to/from JavaScript values. Note that round trips here don't produce the exact same
  // Frankenvalue representation: toJs() puts all the contents together into a single value, and
  // fromJs() always returns a Frakenvalue containing a single V8-serialized value.
  jsg::JsValue toJs(jsg::Lock& js);
  static Frankenvalue fromJs(jsg::Lock& js, jsg::JsValue value);

  // Like toJs() but add the properties to an existing object. Throws if the `Frankenvalue` does
  // not represent an object. This is used to populate `env` in particular.
  void populateJsObject(jsg::Lock& js, jsg::JsObject target);

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

  // ---------------------------------------------------------------------------
  // Capability handling
  //
  // A Frankenvalue can contain capabilities (typically ServiceStubs). When serializing from
  // JavaScript, these will be encoded as integer indexes into a separate table -- the CapTable.

  // The Frakenvalue itself doesn't know how these "capabilities" are implemneted, so leaves this
  // up to a higher layer. It simply manitains a table of `CapTableEntry` objects. `CapTableEntry`
  // serves as a generic base class for multiple representations which serializers and
  // deserializers for specific types will need to support through downcasting.
  //
  // In particular:
  // - Typically, the type is `IoChannelFactory::SubrequestChannel`.
  // - When a Frankenvalue is being used to initialize the `env` of a dynamically-loaded isolate,
  //   each CapTableEntry may simply contain an I/O channel number.
  // - In some environments, a CapTableEntry might some sort of description of how to load a Worker
  //   that implements the capability.
  class CapTableEntry {
   public:
    // Clone the entry, used when `Frakenvalue::clone()` is called. Many implementations may
    // implement this using addRef().
    virtual kj::Own<CapTableEntry> clone() = 0;
  };

  kj::ArrayPtr<kj::Own<CapTableEntry>> getCapTable() {
    return capTable;
  }

  // Rewrite all the caps in the table by calling the `rewrite()` callback on each one.
  template <typename Func>
  void rewriteCaps(Func&& rewrite) {
    for (auto& slot: capTable) {
      slot = rewrite(kj::mv(slot));
    }
  }

  // When deserializing a JS value, the jsg::Deserializer's ExternalHandler will have this type.
  class CapTableReader final: public jsg::Deserializer::ExternalHandler {
   public:
    kj::Maybe<CapTableEntry&> get(uint index) {
      if (index < table.size()) {
        return *table[index];
      } else {
        return kj::none;
      }
    }

   private:
    kj::ArrayPtr<kj::Own<CapTableEntry>> table;
    CapTableReader(kj::ArrayPtr<kj::Own<CapTableEntry>> table): table(table) {}
    friend class Frankenvalue;
  };

  // When serializing a JS value, the jsg::Serializer's ExternalHandler will have this type.
  class CapTableBuilder final: public jsg::Serializer::ExternalHandler {
   public:
    uint add(kj::Own<CapTableEntry> entry) {
      uint result = target.capTable.size();
      target.capTable.add(kj::mv(entry));
      return result;
    }

   private:
    Frankenvalue& target;
    CapTableBuilder(Frankenvalue& target): target(target) {}
    friend class Frankenvalue;
  };

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

  kj::Vector<kj::Own<CapTableEntry>> capTable;

  void fromCapnpImpl(rpc::Frankenvalue::Reader reader, uint& capTablePos);
  void toCapnpImpl(rpc::Frankenvalue::Builder builder, uint capTableSize);
  jsg::JsValue toJsImpl(jsg::Lock& js, kj::ArrayPtr<kj::Own<CapTableEntry>> capTable);
};

// Can't be defined inline since `Frankenvalue` is still incomplete there.
struct Frankenvalue::Property {
  kj::String name;
  Frankenvalue value;

  // `value.capTable` is always empty. Instead, these two values specify the slice of the parent's
  // capTable which this Frankenvalue refers into.
  uint capTableOffset = 0;
  uint capTableSize = 0;
};

}  // namespace workerd
