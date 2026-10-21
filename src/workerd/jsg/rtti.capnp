# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xb042d6da9e1721ad;
# Runtime information about jsg types and definitions

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::jsg::rtti");
$Cxx.allowCancellation;
# TODO: I can't figure out how to make both capnpc-ts and capnpc-cpp generators to see this import
# without code changes. capnpc-ts code is weird:
# https://github.com/jdiaz5513/capnp-ts/blob/master/packages/capnpc-ts/src/generators.ts#L92
# using Modules = import "/workerd/jsg/modules.capnp";

struct Type {
  # A description of a native binding type.
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

    jsgImpl @15 :JsgImplType;
    # jsg implementation type

    jsBuiltin @16: JsBuiltinType;
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

  fullyQualifiedName @1 :Text;

  tsName @2 :Text;
  # Explicit declaration name; empty means derive it from fullyQualifiedName.
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

  name @1 :Text;
}

struct MaybeType {
  # Nullable/optional bindings, including kj::Maybe, jsg::Optional, and Rust wrappers.
  value @0 :Type;

  name @1 :Text;

  allowsNull @2 :Bool;
  # The value may be null in addition to the wrapped type.

  allowsUndefined @3 :Bool;
  # The value may be undefined in addition to the wrapped type.

  # If both flags are false, infer the alternative from name for legacy producers.
  # New producers set at least one flag; a plain value does not need a MaybeType.
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

    v8Function @4;
    # v8::Function

    v8ArrayBuffer @5;
    # v8::ArrayBuffer

    v8Uint16Array @6;
    # v8::Uint16Array
    v8Uint32Array @7;
    # v8::Uint32Array
    v8Int8Array @8;
    # v8::Int8Array
    v8Int16Array @9;
    # v8::Int16Array
    v8Int32Array @10;
    # v8::Int32Array
    v8Float32Array @11;
    # v8::Float32Array
    v8Float64Array @12;
    # v8::Float64Array
    v8BigInt64Array @13;
    # v8::BigInt64Array
    v8BigUint64Array @14;
    # v8::BigUint64Array
    v8Uint8ClampedArray @15;
    # v8::Uint8ClampedArray
    v8Symbol @16;
    # v8::Symbol
    v8TypedArray @17;
    # Any concrete V8 typed array, excluding DataView.
    jsgError @18;
    # A native JavaScript Error.
  }

  type @0 :Type;
}

struct FunctionType {
  # jsg::Function type

  returnType @0 :Type;

  args @1 :List(Type);
}

struct JsgImplType {
  # one of the internal jsg types that are not exposed directly but handled specially

  enum Type {
    configuration @0;
    # api meta configuration object

    v8Isolate @1;

    jsgLock @2;

    jsgTypeHandler @3;

    jsgUnimplemented @4;

    jsgVarargs @5;

    jsgSelfRef @6;

    v8FunctionCallbackInfo @7;

    v8PropertyCallbackInfo @8;

    jsgName @9;
  }

  type @0 :Type;
}

struct Structure {
  # A description of either JSG_RESOURCE or JSG_STRUCT

  name @0 :Text;
  # Structure name

  fullyQualifiedName @5 :Text;
  # Fully-qualified structure name including namespaces and parents

  members @1 :List(Member);
  # All members in declaration order

  extends @2 :Type;
  # base type

  iterable @3 :Bool;
  # true if the structure is iterable
  iterator @6 :Method;
  # Method returning iterator if the structure is iterable

  asyncIterable @4 :Bool;
  # true if the structure is async iterable
  asyncIterator @7 :Method;
  # Method returning async iterator if the structure is async iterable

  disposable @13 :Bool;
  # true if the structure is disposable
  dispose @14 :Method;
  # dispose method

  asyncDisposable @15 :Bool;
  # true if the structure is async disposable
  asyncDispose @16 :Method;
  # asyncDispose method

  tsRoot @8 :Bool;
  # See `JSG_TS_ROOT`'s documentation in the `## TypeScript` section of the JSG README.md.
  # If `JSG_(STRUCT_)TS_ROOT` is declared for a type, this value will be `true`.

  tsOverride @9 :Text;
  # See `JSG_TS_OVERRIDE`'s documentation in the `## TypeScript` section of the JSG README.md.
  # If `JSG_(STRUCT_)TS_OVERRIDE` is declared for a type, this value will be the contents of the
  # macro declaration verbatim.

  tsDefine @10 :Text;
  # See `JSG_TS_DEFINE`'s documentation in the `## TypeScript` section of the JSG README.md.
  # If `JSG_(STRUCT_)TS_DEFINE` is declared for a type, this value will be the contents of the
  # macro declaration verbatim.

  callable @11 :FunctionType;
  # If this type is callable as a function, the signature of said function. Otherwise, null.

  builtinModules @12 :List(Module);
  # List of all builtin modules provided by the context.

  tsName @17 :Text;
  # Explicit declaration name; empty means derive it from fullyQualifiedName.
}

struct Member {
  # One of structure members

  union {
    method @0 :Method;
    # any kind of method

    property @1 :Property;
    # any kind of property

    nested :group {
      structure @2 :Structure;

      name @5 :Text;
      # For JSG_NESTED_TYPE_NAMED, if name is different to structure
    }
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
  fastApiCompatible @4: Bool;
  argNames @5 :List(Text);
  # Names aligned with args; empty entries use the external parameter-name table.
}

struct Property {
  name @0 :Text;
  type @1 :Type;
  readonly @2 :Bool;
  lazy @3 :Bool;
  prototype @4 :Bool;
  getterFastApiCompatible @5: Bool;
  setterFastApiCompatible @6: Bool;
}

struct Constant {
  # static constant in the resource

  name @0 :Text;

  union {
    value @1 :Int64;
    numberValue @2 :Float64;
  }
}

struct Constructor {
  args @0 :List(Type);
  argNames @1 :List(Text);
  # Names aligned with args; empty entries use the external parameter-name table.
}

struct Module {
  specifier @0 :Text;
  # if anyone ever needs module type, it can be implemented by either fixing the Modules reference
  # problem above or copying the original enum.
  # type @1 :Modules.ModuleType;
  union {
    structureName @1 :Text;
    tsDeclarations @2 :Text;
  }
}

struct StructureGroups {
  # Collection of structure groups, consumed by TypeScript definitions generator

  struct StructureGroup {
    # Collection of related structures

    name @0 :Text;

    structures @1 :List(Structure);
  }

  groups @0 :List(StructureGroup);

  modules @1 :List(Module);
}

struct JsBuiltinType {
  # special type for properties whose value is supplied by built-in javascript

  module @0 :Text;
  # module from which the property is imported

  export @1 :Text;
  # export name of the property
}
