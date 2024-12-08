@0xcc3b225cb3101aba;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
$Cxx.allowCancellation;

struct Frankenvalue {
  # Represents a JavaScript value that has been stitched together from multiple sources outside of
  # a JavaScript evaluation context. The Frankevalue can be evaluated down to a JS value as soon
  # as it has a JS execution environment in which to be evaluated.

  union {
    emptyObject @0 :Void;
    # Just an object with no properties.

    json @1 :Text;
    # Parse this JSON-formatted text to compute the value.

    v8Serialized @2 :Data;
    # Parse these V8-serialized bytes to compute the value.
  }

  properties @3 :List(Frankenvalue);
  # Additional properties to add to the value. The base value (specified by the union above) must
  # be an object. Each property in this list must have a `name`. They will be added as properties
  # of the object.
  #
  # If a property in the list concflicts with a property that already exists in the base value,
  # the property is overwritten with the value from the `properties` list.

  name @4 :Text;
  # Property name. Used only when this `Frankenvalue` represents a property, that is, it is an
  # element within the `properties` list of some other `Frankenvalue`. If this is the root value,
  # then `name` must be null.
}
