'use strict';

// The global structuredClone(), installed by main.ts under the
// `typescript_implemented_structured_clone` flag in place of the C++ JSG method
// on ServiceWorkerGlobalScope.prototype.
//
// This module is the WebIDL layer: the receiver check, the argument count, and
// the conversion of the StructuredSerializeOptions dictionary. The clone itself
// is the native serializer's, through utils.structuredClone(), so which values
// clone, which objects transfer, and the errors for either are the same as in
// C++. Where the conversion differs from C++ it follows WebIDL; the differences
// are tabled in src/tests/structured-clone/AGENTS.md.

const {
  ArrayPrototypePush,
  ObjectDefineProperty,
  TypeError: TypeErrorCtor,
} = primordials;

const { structuredClone: nativeStructuredClone } = utils;

// The global proxy, which is the receiver of `globalThis.structuredClone(v)`.
const globalReceiver: unknown = globalThis;

// The C++ texts, kept where the behaviour matches.
const ILLEGAL_INVOCATION =
  'Illegal invocation: function called with incorrect `this` reference. See ' +
  'https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const NO_VALUE =
  "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
  "parameter 1 is not of type 'Value'.";
const BAD_OPTIONS =
  "Failed to execute 'structuredClone' on 'ServiceWorkerGlobalScope': " +
  "parameter 2 is not of type 'StructuredCloneOptions'.";
const BAD_TRANSFER =
  "Incorrect type for the 'transfer' field on 'StructuredCloneOptions': " +
  "the provided value is not of type 'sequence<object>'.";
const BAD_TRANSFER_ITEM =
  "Incorrect type for an element of the 'transfer' field on " +
  "'StructuredCloneOptions': the provided value is not of type 'object'.";

function isObject(value: unknown): value is object {
  return (
    (typeof value === 'object' && value !== null) || typeof value === 'function'
  );
}

// WebIDL's conversion to sequence<object>: any iterable, consumed through its
// own iterator (the user's, by contract), into an array of our own that the
// native side can read without running user code.
function toObjectSequence(value: unknown): object[] {
  if (!isObject(value)) {
    throw new TypeErrorCtor(BAD_TRANSFER);
  }
  const iterable = value as { [Symbol.iterator]?: unknown };
  if (typeof iterable[primordials.SymbolIterator] !== 'function') {
    throw new TypeErrorCtor(BAD_TRANSFER);
  }
  const list: object[] = [];
  for (const item of iterable as Iterable<unknown>) {
    if (!isObject(item)) {
      throw new TypeErrorCtor(BAD_TRANSFER_ITEM);
    }
    ArrayPrototypePush(list, item);
  }
  return list;
}

// A method, so the function has no `prototype` and is not a constructor, like
// the JSG method it replaces.
const { structuredClone } = {
  structuredClone(this: unknown, value: unknown, options?: unknown): unknown {
    // WebIDL: an operation of the global takes undefined or null as the
    // global itself.
    if (this !== undefined && this !== null && this !== globalReceiver) {
      throw new TypeErrorCtor(ILLEGAL_INVOCATION);
    }
    if (arguments.length === 0) {
      throw new TypeErrorCtor(NO_VALUE);
    }
    let transfer: object[] | undefined;
    if (options !== undefined && options !== null) {
      if (!isObject(options)) {
        throw new TypeErrorCtor(BAD_OPTIONS);
      }
      const list = (options as { transfer?: unknown }).transfer;
      if (list !== undefined) {
        transfer = toObjectSequence(list);
      }
    }
    return nativeStructuredClone(value, transfer);
  },
};

// WebIDL: the optional options argument does not count.
ObjectDefineProperty(structuredClone, 'length', {
  __proto__: null,
  configurable: true,
  enumerable: false,
  writable: false,
  value: 1,
});

module.exports = { structuredClone };
