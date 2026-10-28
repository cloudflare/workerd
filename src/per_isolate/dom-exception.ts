'use strict';

// The global DOMException (https://webidl.spec.whatwg.org/#idl-DOMException).
//
// A DOMException looks as the C++ JSG class's instances do (src/workerd/jsg/dom-exception.h):
// own read-only `code`, `name` and `message` properties, an enumerable own `stack`, and the
// legacy code constants on both the constructor and the prototype. Native code creates
// DOMExceptions without running this constructor (jsg::newDOMException()), over the prototype
// that install() hands it, and recognizes them by the brand that utils.initDOMException() sets.
// This constructor sets the same brand, so utils.isDOMException() is true for either kind, and
// for a subclass instance.
//
// DOMException is a function rather than a class so that its `prototype` property is
// writable, as a JSG class's is unless the immutable_api_prototypes flag is set.

const {
  ErrorCaptureStackTrace,
  ErrorPrototype,
  ObjectCreate,
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectGetPrototypeOf,
  SymbolFor,
  SymbolToStringTag,
  TypeError,
} = primordials;

// The legacy codes, in the order JSG defines the constants
// (JSG_DOM_EXCEPTION_FOR_EACH_ERROR_NAME).
const LEGACY_CODES: readonly (readonly [string, number])[] = [
  ['INDEX_SIZE_ERR', 1],
  ['DOMSTRING_SIZE_ERR', 2],
  ['HIERARCHY_REQUEST_ERR', 3],
  ['WRONG_DOCUMENT_ERR', 4],
  ['INVALID_CHARACTER_ERR', 5],
  ['NO_DATA_ALLOWED_ERR', 6],
  ['NO_MODIFICATION_ALLOWED_ERR', 7],
  ['NOT_FOUND_ERR', 8],
  ['NOT_SUPPORTED_ERR', 9],
  ['INUSE_ATTRIBUTE_ERR', 10],
  ['INVALID_STATE_ERR', 11],
  ['SYNTAX_ERR', 12],
  ['INVALID_MODIFICATION_ERR', 13],
  ['NAMESPACE_ERR', 14],
  ['INVALID_ACCESS_ERR', 15],
  ['VALIDATION_ERR', 16],
  ['TYPE_MISMATCH_ERR', 17],
  ['SECURITY_ERR', 18],
  ['NETWORK_ERR', 19],
  ['ABORT_ERR', 20],
  ['URL_MISMATCH_ERR', 21],
  ['QUOTA_EXCEEDED_ERR', 22],
  ['TIMEOUT_ERR', 23],
  ['INVALID_NODE_TYPE_ERR', 24],
  ['DATA_CLONE_ERR', 25],
];

// The message and name are converted as Web IDL converts a DOMString, which a template literal
// does too: a Symbol throws a TypeError.
function DOMException(this: object, ...args: unknown[]): void {
  if (new.target === undefined) {
    throw new TypeError(
      "Failed to construct 'DOMException': Please use the 'new' operator, this object constructor cannot be called as a function."
    );
  }
  const message = args[0] === undefined ? '' : `${args[0] as string}`;
  const name = args[1] === undefined ? 'Error' : `${args[1] as string}`;
  utils.initDOMException(this, name, message);
  // Leaves out this constructor's frame, but not a subclass constructor's.
  ErrorCaptureStackTrace(this, DOMException);
  // Error properties are not enumerable, but a DOMException's always have been.
  ObjectDefineProperty(this, 'stack', { __proto__: null, enumerable: true });
}

// Defines `key` on `target` with the attributes it has on `source`, which JSG chooses by
// compatibility flags, and `value`.
function copyProperty(
  target: object,
  source: object,
  key: string | symbol,
  value: unknown
): void {
  const descriptor = ObjectGetOwnPropertyDescriptor(source, key);
  if (descriptor === undefined) {
    throw new TypeError(`DOMException has no ${String(key)} to copy`);
  }
  ObjectDefineProperty(target, key, { ...descriptor, __proto__: null, value });
}

// Gives DOMException the properties of `native`, the C++ class it replaces, with their
// attributes and in their order.
function setUpClass(native: Function): void {
  const nativePrototype = native.prototype as object;
  copyProperty(DOMException, native, 'length', 0);
  copyProperty(DOMException, native, 'name', 'DOMException');

  const prototype = ObjectCreate(ErrorPrototype) as object;
  for (let i = 0; i < LEGACY_CODES.length; i++) {
    const [name, code] = LEGACY_CODES[i] as readonly [string, number];
    copyProperty(DOMException, native, name, code);
    copyProperty(prototype, nativePrototype, name, code);
  }
  copyProperty(prototype, nativePrototype, 'constructor', DOMException);
  // Present under the set_tostring_tag flag.
  if (
    ObjectGetOwnPropertyDescriptor(nativePrototype, SymbolToStringTag) !==
    undefined
  ) {
    copyProperty(prototype, nativePrototype, SymbolToStringTag, 'DOMException');
  }
  // Tools recognize a runtime class by this symbol.
  const internalClass = SymbolFor('cloudflare:internal-class');
  copyProperty(prototype, nativePrototype, internalClass, internalClass);
  // util.inspect() formats an instance of a runtime class through this symbol.
  const inspect = utils.getApiSymbol('kResourceTypeInspect');
  copyProperty(
    prototype,
    nativePrototype,
    inspect,
    ObjectFreeze({ __proto__: null })
  );

  // Not writable under the immutable_api_prototypes flag.
  copyProperty(DOMException, native, 'prototype', prototype);
}

// Replaces the C++ class as the global DOMException, keeping the property where JSG defines it
// (on the global object's prototype) and its attributes, and has native code create instances
// of this class from now on.
function install(): void {
  let owner: object | null = globalThis;
  while (owner !== null) {
    const descriptor = ObjectGetOwnPropertyDescriptor(owner, 'DOMException');
    if (descriptor !== undefined) {
      setUpClass(descriptor.value as Function);
      ObjectDefineProperty(owner, 'DOMException', {
        ...descriptor,
        __proto__: null,
        value: DOMException,
      });
      utils.setDOMExceptionPrototype(DOMException.prototype as object);
      return;
    }
    owner = ObjectGetPrototypeOf(owner) as object | null;
  }
  throw new TypeError('the global object has no DOMException');
}

module.exports = {
  install,
};
