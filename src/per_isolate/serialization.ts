'use strict';

// The protocol by which the classes implemented here take part in structured
// serialization (structuredClone(), postMessage(), RPC, Durable Object
// storage), modelled on Node.js's js_transferable. jsg/ser.h describes the
// C++ side and the format, and "Classes Implemented in JavaScript" in
// docs/jsg.md has a complete example of a cloneable class. A leaf module,
// which main.ts loads whatever the compatibility flags.
//
// An instance markUncloneable() marks throws DataCloneError when serialized,
// unless the serialization takes it as an external (a stream over RPC). Only
// a marked instance is offered as an external, so a class whose instances
// are externals must mark each one. One markCloneable() marks with its
// class's Cloneable, which makeCloneable() returns, is serialized as
// makeCloneable() says, whatever its prototype is by then:
//
// - deserializeInfo names the class's constructor as 'module:Export', an
//   export of a module the bootstrap loads. Serialized data, including
//   Durable Object storage, holds it, so it can never change: keep a renamed
//   class's old export as an alias. The deserializer finds only modules
//   already loaded, and can't load one (V8 runs no JavaScript while it
//   reads), so the module must be required while the bootstrap runs, not
//   lazily. The export must be a data property, as
//   `module.exports = { ... }` makes it: the deserializer can't call a
//   getter, so data naming one can't be read.
// - Whether the module is loaded, not what the compatibility flags expose,
//   decides whether a reader can read the class: data naming a class whose
//   module isn't loaded can't be read, and data from an untrusted source,
//   such as an RPC peer, can make an instance of any class whose module is.
//   A reader may have different flags from the writer. Load the module
//   whatever the flags if every reader must read the class, as when it
//   replaces a C++ class under a flag, so that data written either way can
//   be read either way. Load the module of a class that only some flags
//   expose, such as an experimental one, only under those flags, as main.ts
//   does CloneableTestBox's.
// - clone(instance) returns the instance's data: any serializable value,
//   which may refer to the instance itself.
// - deserialize(instance, data) initializes an instance from the data.
//
// Either mark is ignored on an object whose prototype is Object.prototype or
// null: to keep plain objects cheap, the serializer does not check them for
// one. Mark instances of a class, or of a prototype of the runtime's own.
// User code can give an instance either prototype, and so have it copied as a
// plain object, with its own enumerable string-keyed properties. Keep an
// instance's state out of those: in #private fields or a WeakMap for an
// uncloneable class, and under private symbols for a cloneable one.
// Either is ignored, too, on an instance of a built-in that V8 serializes
// itself, even of a subclass: an Array, ArrayBuffer, SharedArrayBuffer,
// typed array, DataView, Date, Map, Set, RegExp, or Boolean, Number or String
// object is cloned as the built-in. makeCloneable() rejects a class that
// inherits from one.
//
// The deserializer makes the instance from Export.prototype WITHOUT calling
// the constructor (making a native error if the prototype inherits from
// Error.prototype), marks it with the class's Cloneable, and calls
// deserialize() once the whole value has been read, in the order the
// instances were read. So:
//
// - Keep the state deserialize() restores under private symbols
//   (utils.getPrivateSymbol), not #private fields, which an instance made
//   without the constructor lacks; and brand-check it the same way, not with
//   `#field in`.
// - Define the instance's other properties, such as an error's `message`,
//   with ObjectDefineProperty(), not `=`, which calls any setter on the
//   prototype chain, where user code may have installed one: on the class's
//   prototype if user code can reach it, or on Error.prototype. Assigning
//   with a private symbol is safe: V8 never looks one up the prototype chain.
// - Validate the data completely: it may come from an untrusted source, such
//   as an RPC peer, and need not be anything clone() returned. Check the type
//   of every value read from it: over RPC, a value in the data may be a stub,
//   which reports every property as present. Don't keep anything derived from
//   it that later reaches a native binding unchecked.
// - Use primordials only: deserialize() runs on an instance whose prototype
//   and globals user code may have changed. Read the data's properties with
//   getOwnDataProperty(), not `data.x` or destructuring, which look a
//   property the data lacks up on its prototype chain, where user code may
//   have installed a getter.
// - Don't use other objects in the data: their deserialize() may not have
//   run yet.
// - Leave an error's `stack` out of the data, and don't set it in
//   deserialize(). The serializer writes it if it is a string, and the
//   deserializer restores it, as it does a native error's, or else keeps the
//   stack it captured, which begins with the error's name and the writer's
//   message. Still set `message` in deserialize(): the serializer writes the
//   message only for the stack.

const {
  Array,
  ArrayBuffer,
  ArrayPrototypeIncludes,
  Boolean,
  DataView,
  Date,
  Map,
  Number,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectGetPrototypeOf,
  ObjectHasOwn,
  ObjectSetPrototypeOf,
  RegExp,
  Set,
  String,
  TypedArrayPrototype,
  TypeError,
} = primordials;
const { getPrivateSymbol } = utils;

const kSerializeMode = getPrivateSymbol('workerd:serializeMode');
const kCloneable = getPrivateSymbol('workerd:cloneable');

// The mark of an uncloneable instance. Any mark but a Cloneable would do.
const kUncloneable = 0;

// The prototypes of the built-ins that V8 serializes itself, never offering
// an instance to the serializer, which would read its mark. SharedArrayBuffer
// is not defined in every context the bootstrap runs in (not while it makes a
// startup snapshot), and a class here can inherit from it only where it is.
const kNativelySerializedPrototypes: readonly object[] = [
  Array.prototype,
  ArrayBuffer.prototype,
  ...(typeof SharedArrayBuffer === 'function'
    ? [SharedArrayBuffer.prototype]
    : []),
  TypedArrayPrototype,
  DataView.prototype,
  Date.prototype,
  Map.prototype,
  Set.prototype,
  RegExp.prototype,
  Boolean.prototype,
  Number.prototype,
  String.prototype,
];

// How the instances of a class are serialized: the class's descriptor in
// jsg/ser.h, whose own properties C++ reads. A null-prototype record, so that
// reading it finds no Object.prototype property, which makeCloneable()
// freezes: the serializer reads a descriptor's properties once, and rejects
// one whose clone or deserializeInfo could change.
class Cloneable<T extends object> {
  declare deserializeInfo: string;
  declare clone: (instance: T) => unknown;
  declare deserialize: (instance: T, data: unknown) => void;
  constructor(
    deserializeInfo: string,
    clone: (instance: T) => unknown,
    deserialize: (instance: T, data: unknown) => void
  ) {
    this.deserializeInfo = deserializeInfo;
    this.clone = clone;
    this.deserialize = deserialize;
  }
}
ObjectSetPrototypeOf(Cloneable.prototype, null);
delete (Cloneable.prototype as { constructor?: unknown }).constructor;
ObjectFreeze(Cloneable.prototype);

// Marks `instance` as not serializable.
function markUncloneable(instance: object): void {
  (instance as Record<symbol, unknown>)[kSerializeMode] = kUncloneable;
}

// Marks `instance` as serializable as `cloneable` says.
function markCloneable<T extends object>(
  instance: T,
  cloneable: Cloneable<T>
): void {
  (instance as Record<symbol, unknown>)[kSerializeMode] = cloneable;
}

// Returns the Cloneable with which the constructor of `ctor` marks each
// instance: see the top of this file. An instance of a subclass is
// serialized as one of `ctor`. Throws if `ctor` inherits from a built-in that
// V8 serializes itself.
function makeCloneable<T extends object>(
  ctor: abstract new (...args: never[]) => T,
  deserializeInfo: string,
  clone: (instance: T) => unknown,
  deserialize: (instance: T, data: unknown) => void
): Cloneable<T> {
  for (
    let proto: object | null = ctor.prototype;
    proto !== null;
    proto = ObjectGetPrototypeOf(proto)
  ) {
    if (ArrayPrototypeIncludes(kNativelySerializedPrototypes, proto)) {
      throw new TypeError(
        `${deserializeInfo} inherits from a built-in that V8 serializes ` +
          'itself, so it cannot be cloneable.'
      );
    }
  }
  const cloneable = ObjectFreeze(
    new Cloneable(deserializeInfo, clone, deserialize)
  );
  (ctor.prototype as Record<symbol, unknown>)[kCloneable] = cloneable;
  return cloneable;
}

// Returns the value of `data`'s own data property `key`, or undefined if it
// has none, or has an accessor instead. Reads neither the prototype chain nor
// the property descriptor's, and calls no getter. Runs no user code unless
// `data` is a Proxy, which deserialized data never is. Over RPC, it can return
// a stub whatever `key` is, so check the type of what it returns.
function getOwnDataProperty(data: object, key: string): unknown {
  const descriptor = ObjectGetOwnPropertyDescriptor(data, key);
  if (descriptor === undefined || !ObjectHasOwn(descriptor, 'value')) {
    return undefined;
  }
  return descriptor.value;
}

export type { Cloneable };

export type SerializationHelpers = {
  markUncloneable: typeof markUncloneable;
  markCloneable: typeof markCloneable;
  makeCloneable: typeof makeCloneable;
  getOwnDataProperty: typeof getOwnDataProperty;
};

module.exports = ObjectFreeze({
  markUncloneable,
  markCloneable,
  makeCloneable,
  getOwnDataProperty,
} satisfies SerializationHelpers);
