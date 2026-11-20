'use strict';

// Helpers that make a bootstrap class look as a JSG resource type does.

const {
  ObjectDefineProperties,
  ObjectFreeze,
  SymbolFor,
  SymbolToStringTag,
  TypeError,
  kIllegalInvocation,
} = primordials;

const { getApiSymbol } = utils;

const kInternalClass = SymbolFor('cloudflare:internal-class');
const kResourceTypeInspect = getApiSymbol('kResourceTypeInspect');
const kNoInspectProperties = ObjectFreeze({ __proto__: null });

function setNameAndLength(fn: unknown, name: string, length: number): void {
  ObjectDefineProperties(fn, {
    __proto__: null,
    name: { __proto__: null, value: name },
    length: { __proto__: null, value: length },
  });
}

// What JSG puts on every resource type's prototype, in JSG's order: under
// set_tostring_tag, the tag; the marker tools use to recognize runtime
// classes; and util.inspect()'s formatting hook, which formats an instance
// from its accessors.
function markAsRuntimeClass(proto: object, name: string): void {
  if (compatFlags['set_tostring_tag'] === true) {
    ObjectDefineProperties(proto, {
      __proto__: null,
      [SymbolToStringTag]: {
        __proto__: null,
        value: name,
        writable: true,
        enumerable: false,
        configurable: true,
      },
    });
  }
  ObjectDefineProperties(proto, {
    __proto__: null,
    [kInternalClass]: {
      __proto__: null,
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    },
    [kResourceTypeInspect]: {
      __proto__: null,
      value: kNoInspectProperties,
      writable: false,
      enumerable: false,
      configurable: true,
    },
  });
}

// The value an instance of a bootstrap class holds under the V8-private
// `slot`, which user code can neither read nor forge. Throws JSG's
// illegal-invocation TypeError for any other receiver, a proxy of an instance
// included, as a JSG member does.
function unwrapReceiver<T>(self: unknown, slot: symbol): T {
  const value =
    typeof self === 'object' && self !== null
      ? (self as Record<symbol, unknown>)[slot]
      : undefined;
  if (value === undefined) throw new TypeError(kIllegalInvocation);
  return value as T;
}

export type RuntimeClassHelpers = {
  markAsRuntimeClass: typeof markAsRuntimeClass;
  setNameAndLength: typeof setNameAndLength;
  unwrapReceiver: typeof unwrapReceiver;
};

module.exports = {
  markAsRuntimeClass,
  setNameAndLength,
  unwrapReceiver,
};
