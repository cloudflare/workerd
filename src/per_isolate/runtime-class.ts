'use strict';

// Helpers that make a bootstrap class look as a JSG resource type does.

const { ObjectDefineProperties, ObjectFreeze, SymbolFor, SymbolToStringTag } =
  primordials;

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

// What JSG puts on every resource type's prototype, in JSG's order: the tag;
// the marker tools use to recognize runtime classes; and util.inspect()'s
// formatting hook, which formats an instance from its accessors.
//
// The tag is writable, as JSG's is. WebIDL makes it non-writable, and the
// divergence is intentional: a bootstrap class stands in for a C++ one, and
// looks as it does. JSG sets the tag only under set_tostring_tag, but this
// sets it whatever the flags say, so that a class marked in the main phase,
// which must not depend on the flags, is the same in every context.
// removeToStringTagUnlessFlagged() takes it off again in a context without
// the flag.
function markAsRuntimeClass(proto: object, name: string): void {
  ObjectDefineProperties(proto, {
    __proto__: null,
    [SymbolToStringTag]: {
      __proto__: null,
      value: name,
      writable: true,
      enumerable: false,
      configurable: true,
    },
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

// Without set_tostring_tag JSG sets no tag, so this removes the one on
// `proto`. It reads the flags, so it may run only in the post-snapshot
// phase (main-post.ts) or in a module that loads after it.
function removeToStringTagUnlessFlagged(proto: object): void {
  if (utils.getCompatFlags()['set_tostring_tag'] !== true) {
    delete (proto as Record<symbol, unknown>)[SymbolToStringTag];
  }
}

export type RuntimeClassHelpers = {
  markAsRuntimeClass: typeof markAsRuntimeClass;
  removeToStringTagUnlessFlagged: typeof removeToStringTagUnlessFlagged;
  setNameAndLength: typeof setNameAndLength;
};

module.exports = {
  markAsRuntimeClass,
  removeToStringTagUnlessFlagged,
  setNameAndLength,
};
