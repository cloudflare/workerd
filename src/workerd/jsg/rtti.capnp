# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xb042d6da9e1721ad;
# Runtime information about jsg types and definitions

$import "/capnp/c++.capnp".namespace("workerd::jsg::rtti");

struct Type {
  # A description of the C++ type.
  # It is as precise as needed for applications, and is mostly how the type looks from the js side.

  union {
    unknown @0 :Void;
    # statically unknown type

    voidt @1 :Void;
    # void type

    boolt @2 :Void;
    # boolean type

    number @3 :NumberType;
    # number type

    promise @4 :PromiseType;
    # jsg, kj Promise

    structure @5 :StructureType;
    # jsg resource or struct

    string @6 :StringType;
    # any string-like type

    object @7 :Void;
    # generic object type

    array @8 :ArrayType;
    # Array or ArrayPtr

    maybe @9 :MaybeType;
    # kj::Maybe or jsg::Optional

    dict @10 :DictType;
    # jsg::Dict

    oneOf @11: OneOfType;
    # kj::OneOf

    builtin @12 :BuiltinType;
    # one of the builtin types

    intrinsic @13 :IntrinsicType;
    # one of v8 intrinsics

    function @14 :FunctionType;
    # jsg::Function
  }
}

struct NumberType {
  # Any c++ number type
  name @0 :Text;
}

struct PromiseType {
  # kj or jsg Promise<T>
  value @0 :Type;
}

struct StructureType {
  # Structure types need to be resolved separately to prevent circular references with types

  name @0 :Text;
}

struct StringType {
  # any string or string-like type
  name @0 :Text;
}

struct IntrinsicType {
  # v8::Intrinsic

  name @0 :Text;
}

struct ArrayType {
  # Array like structure
  element @0 :Type;
}

struct MaybeType {
  # kj::Maybe or jsg::Optional
  value @0 :Type;
}

struct DictType {
  # jsg::dict
  key @0 :Type;
  value @1 :Type;
}

struct OneOfType {
  # kj::OneOf
  variants @0 :List(Type);
}

struct BuiltinType {
  # One of the types provided by the JS or Runtime platform.

  enum Type {
    v8Uint8Array @0;
    # v8::UInt8Array

    v8ArrayBufferView @1;
    # v8::ArrayBufferView

    jsgBufferSource @2;
    # BufferSource

    kjDate @3;
    # kj::Date

    jsgUnimplemented @4;
    # jsg::Unimplemented

    v8Isolate @5;
    # v8::Isolate

    jsgVarargs @6;
    # jsg::Varargs;

    v8Function @7;
    # v8::Function

    flags @8;
    # CompatibilityFlags::Reader

    jsgLock @9;
    # jsg::Lock

    jsgTypeHandler @10;
    # jsg::TypeHandler
  }

  type @0 :Type;
}

struct FunctionType {
  # jsg::Function type

  returnType @0 :Type;

  args @1 :List(Type);
}

struct Structure {
  # A description of either JSG_RESOURCE or JSG_STRUCT

  name @0 :Text;
  # Structure name

  members @1 :List(Member);
  # All members in declaration order

  extends @2 :Type;
  # base type

  iterable @3 :Bool;
  # true if the structure is iterable

  asyncIterable @4 :Bool;
  # true if the structure is async iterable
}

struct Member {
  # One of structure members

  union {
    method @0 :Method;
    # any kind of method

    property @1 :Property;
    # any kind of property

    nested @2 :Structure;
    # nested type

    constant @3 :Constant;
    # static constant

    constructor @4 :Constructor;
    # structure constructor
  }
}

struct Method {
  name @0 :Text;
  returnType @1 :Type;
  args @2 :List(Type);
  static @3 :Bool;
}

struct Property {
  name @0 :Text;
  type @1 :Type;
  readonly @2 :Bool;
  lazy @3 :Bool;
  prototype @4 :Bool;
}

struct Constant {
  # static constant in the resource

  name @0 :Text;

  value @1 :Int64;
  # TODO: we may need a union here
}

struct Constructor {
  args @0 :List(Type);
}
