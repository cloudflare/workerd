'use strict';

// The DOMException the bootstrap's modules create and recognize, and that
// main.ts installs as the global under the TS_DOM_EXCEPTION autogate: the
// class below (WebIDL sections 3.14.1 and 4.3). Without the autogate it is
// the C++ class (jsg/dom-exception.h) that the global scope installs,
// captured as this module loads, before user code can replace the global.
// main.ts loads it before any other module that may use it.
//
// An instance is an API wrapper object (jsg::getBootstrapDomExceptionBase())
// with V8's stack, whose prototype is DOMException.prototype; Error.isError()
// reports it as a native error, as WebIDL requires. Its brand, name and
// message live under V8 private symbols, and the prototype carries a fourth,
// which native code also uses (jsg/dom-exception.h): Lock::domException()
// creates instances without running JavaScript, and the serializer and
// tryUnwrap() recognize them by the brand, which the attributes also check.
// The name and message are under the keys that jsg.h gives any error whose
// name and message are accessors, so that native code can describe it for a
// log. Native code finds the class through this module's exports.
//
// Under the autogate, the module also exports QuotaExceededError (WebIDL
// section 2.8.3), the one DOMException-derived interface WebIDL predefines.
// An instance is a DOMException that also holds its quota and requested
// amount under private symbols. It is cloneable (serialization.ts) as
// 'dom-exception:QuotaExceededError', which serialized data holds, so the
// module and export names can never change. Native code knows nothing of the
// class. The C++ implementation has none, so a reader without the autogate
// can't read one.

import type { SerializationHelpers } from './serialization';

const {
  Error: ErrorCtor,
  ErrorCaptureStackTrace,
  NumberIsFinite,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectSetPrototypeOf,
  ReflectConstruct,
  SafeMap,
  SymbolFor,
  SymbolToStringTag,
  RangeError: RangeErrorCtor,
  TypeError: TypeErrorCtor,
} = primordials;

const { getPrivateSymbol, DOMExceptionBase } = utils;
const { getOwnDataProperty, makeCloneable, markCloneable } =
  require('serialization') as SerializationHelpers;

const kBrand = getPrivateSymbol('workerd:domException');
const kName = getPrivateSymbol('workerd:errorName');
const kMessage = getPrivateSymbol('workerd:errorMessage');
const kPrototype = getPrivateSymbol('workerd:domExceptionPrototype');
// A QuotaExceededError's quota and requested amount, each a number or null.
// The quota is also its brand.
const kQuota = getPrivateSymbol('workerd:quotaExceededErrorQuota');
const kRequested = getPrivateSymbol('workerd:quotaExceededErrorRequested');

type Slots = Record<symbol, unknown>;

// JSG's mark on the prototype of every runtime class, by which Miniflare
// recognizes them (jsg/resource.h). JSG's other mark, kResourceTypeInspect,
// is left out, so that util.inspect() renders an instance as the error it
// is, as Node renders its DOMException.
const kInternalClass = SymbolFor('cloudflare:internal-class');

// The legacy code constants, in the order WebIDL declares them, with the
// name each one is the code of. DOMStringSizeError, NoDataAllowedError and
// ValidationError are not in WebIDL's table of names, so their code is 0
// though their constants remain.
const kLegacyCodes: [string, number, string | null][] = [
  ['INDEX_SIZE_ERR', 1, 'IndexSizeError'],
  ['DOMSTRING_SIZE_ERR', 2, null],
  ['HIERARCHY_REQUEST_ERR', 3, 'HierarchyRequestError'],
  ['WRONG_DOCUMENT_ERR', 4, 'WrongDocumentError'],
  ['INVALID_CHARACTER_ERR', 5, 'InvalidCharacterError'],
  ['NO_DATA_ALLOWED_ERR', 6, null],
  ['NO_MODIFICATION_ALLOWED_ERR', 7, 'NoModificationAllowedError'],
  ['NOT_FOUND_ERR', 8, 'NotFoundError'],
  ['NOT_SUPPORTED_ERR', 9, 'NotSupportedError'],
  ['INUSE_ATTRIBUTE_ERR', 10, 'InUseAttributeError'],
  ['INVALID_STATE_ERR', 11, 'InvalidStateError'],
  ['SYNTAX_ERR', 12, 'SyntaxError'],
  ['INVALID_MODIFICATION_ERR', 13, 'InvalidModificationError'],
  ['NAMESPACE_ERR', 14, 'NamespaceError'],
  ['INVALID_ACCESS_ERR', 15, 'InvalidAccessError'],
  ['VALIDATION_ERR', 16, null],
  ['TYPE_MISMATCH_ERR', 17, 'TypeMismatchError'],
  ['SECURITY_ERR', 18, 'SecurityError'],
  ['NETWORK_ERR', 19, 'NetworkError'],
  ['ABORT_ERR', 20, 'AbortError'],
  ['URL_MISMATCH_ERR', 21, 'URLMismatchError'],
  ['QUOTA_EXCEEDED_ERR', 22, 'QuotaExceededError'],
  ['TIMEOUT_ERR', 23, 'TimeoutError'],
  ['INVALID_NODE_TYPE_ERR', 24, 'InvalidNodeTypeError'],
  ['DATA_CLONE_ERR', 25, 'DataCloneError'],
];

const kCodeForName = new SafeMap() as Map<string, number>;
for (const [, code, name] of kLegacyCodes) {
  if (name !== null) kCodeForName.set(name, code);
}

function slotsOf(value: unknown): Slots {
  if (
    typeof value !== 'object' ||
    value === null ||
    (value as Slots)[kBrand] === undefined
  ) {
    throw new TypeErrorCtor('Illegal invocation');
  }
  return value as Slots;
}

class DOMException {
  constructor(message: unknown = '', name: unknown = 'Error') {
    // DOMString conversions, message first. A template literal, unlike
    // String(), throws for a Symbol, as ToString does.
    const messageString = `${message as string}`;
    const nameString = `${name as string}`;
    // An API wrapper object with new.target's prototype, so subclasses work,
    // and V8's stack, whose frames start at the caller of new.target. V8
    // formats the stack's header lazily, from the name and message read
    // through the getters below.
    const self = ReflectConstruct(DOMExceptionBase, [], new.target) as Slots;
    self[kBrand] = true;
    self[kName] = nameString;
    self[kMessage] = messageString;
    ErrorCaptureStackTrace(self, new.target);
    return self as unknown as DOMException;
  }

  get name(): string {
    return slotsOf(this)[kName] as string;
  }

  get message(): string {
    return slotsOf(this)[kMessage] as string;
  }

  get code(): number {
    return kCodeForName.get(slotsOf(this)[kName] as string) ?? 0;
  }
}

function quotaSlotsOf(value: unknown): Slots {
  if (
    typeof value !== 'object' ||
    value === null ||
    (value as Slots)[kQuota] === undefined
  ) {
    throw new TypeErrorCtor('Illegal invocation');
  }
  return value as Slots;
}

const kConstructQuota = "Failed to construct 'QuotaExceededError': ";

// A member of the QuotaExceededErrorOptions dictionary, converted to a
// WebIDL double, or null if it is not present.
function toAmount(value: unknown, member: string): number | null {
  if (value === undefined) return null;
  // ToNumber, which throws for a Symbol or a BigInt, as Number() does not.
  const amount = +(value as number);
  if (!NumberIsFinite(amount)) {
    throw new TypeErrorCtor(
      `${kConstructQuota}Failed to read the '${member}' property from ` +
        "'QuotaExceededErrorOptions': The provided double value is non-finite."
    );
  }
  return amount;
}

class QuotaExceededError extends DOMException {
  constructor(message: unknown = '', options: unknown = undefined) {
    // The arguments are converted in order, the dictionary's members in
    // lexicographic order, before any of the constructor steps.
    const messageString = `${message as string}`;
    let quota: number | null = null;
    let requested: number | null = null;
    if (options !== undefined && options !== null) {
      if (typeof options !== 'object' && typeof options !== 'function') {
        throw new TypeErrorCtor(
          `${kConstructQuota}The provided value is not of type ` +
            "'QuotaExceededErrorOptions'."
        );
      }
      const dictionary = options as { quota?: unknown; requested?: unknown };
      quota = toAmount(dictionary.quota, 'quota');
      requested = toAmount(dictionary.requested, 'requested');
    }
    if (quota !== null && quota < 0) {
      throw new RangeErrorCtor(`${kConstructQuota}The quota is negative.`);
    }
    if (requested !== null && requested < 0) {
      throw new RangeErrorCtor(
        `${kConstructQuota}The requested amount is negative.`
      );
    }
    if (quota !== null && requested !== null && requested < quota) {
      throw new RangeErrorCtor(
        `${kConstructQuota}The requested amount is less than the quota.`
      );
    }
    super(messageString, 'QuotaExceededError');
    const self = this as unknown as Slots;
    self[kQuota] = quota;
    self[kRequested] = requested;
    markCloneable(this, quotaExceededErrorCloneable);
  }

  get quota(): number | null {
    return quotaSlotsOf(this)[kQuota] as number | null;
  }

  get requested(): number | null {
    return quotaSlotsOf(this)[kRequested] as number | null;
  }
}

// Whether `value` is a QuotaExceededError's quota or requested amount, as
// the constructor stores it.
function isAmount(value: unknown): value is number | null {
  return (
    value === null ||
    (typeof value === 'number' && NumberIsFinite(value) && value >= 0)
  );
}

// The data is the error's message, quota and requested amount. The
// serializer writes the stack.
const quotaExceededErrorCloneable = makeCloneable(
  QuotaExceededError,
  'dom-exception:QuotaExceededError',
  (error) => {
    const slots = quotaSlotsOf(error);
    return {
      message: slots[kMessage],
      quota: slots[kQuota],
      requested: slots[kRequested],
    };
  },
  (error, data) => {
    if (typeof data !== 'object' || data === null) {
      throw invalidQuotaExceededErrorData();
    }
    const message = getOwnDataProperty(data, 'message');
    const quota = getOwnDataProperty(data, 'quota');
    const requested = getOwnDataProperty(data, 'requested');
    // As the constructor allows them.
    if (
      typeof message !== 'string' ||
      !isAmount(quota) ||
      !isAmount(requested) ||
      (quota !== null && requested !== null && requested < quota)
    ) {
      throw invalidQuotaExceededErrorData();
    }
    // In the constructors' order.
    const slots = error as unknown as Slots;
    slots[kBrand] = true;
    slots[kName] = 'QuotaExceededError';
    slots[kMessage] = message;
    slots[kQuota] = quota;
    slots[kRequested] = requested;
  }
);

function invalidQuotaExceededErrorData(): DOMException {
  return new DOMException(
    'Deserialization failed: invalid QuotaExceededError data.',
    'DataCloneError'
  );
}

function constants(): PropertyDescriptorMap {
  const descriptors: PropertyDescriptorMap = {};
  for (const [constant, code] of kLegacyCodes) {
    descriptors[constant] = {
      __proto__: null,
      value: code,
      enumerable: true,
      writable: false,
      configurable: false,
    } as PropertyDescriptor;
  }
  return descriptors;
}

function install(): typeof DOMException {
  const proto = DOMException.prototype;
  ObjectSetPrototypeOf(proto, ErrorCtor.prototype);
  (proto as unknown as Record<symbol, unknown>)[kPrototype] = true;
  // WebIDL attributes are enumerable; class accessors are not.
  ObjectDefineProperties(proto, {
    __proto__: null,
    [kInternalClass]: {
      __proto__: null,
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    },
    name: { __proto__: null, enumerable: true },
    message: { __proto__: null, enumerable: true },
    code: { __proto__: null, enumerable: true },
  });
  ObjectDefineProperties(DOMException, constants());
  ObjectDefineProperties(proto, constants());
  if (compatFlags['set_tostring_tag']) {
    ObjectDefineProperties(proto, {
      [SymbolToStringTag]: {
        __proto__: null,
        value: 'DOMException',
        configurable: true,
      },
    });
  }
  return DOMException;
}

function installQuotaExceededError(): typeof QuotaExceededError {
  const proto = QuotaExceededError.prototype;
  ObjectDefineProperties(proto, {
    __proto__: null,
    [kInternalClass]: {
      __proto__: null,
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    },
    quota: { __proto__: null, enumerable: true },
    requested: { __proto__: null, enumerable: true },
  });
  if (compatFlags['set_tostring_tag']) {
    ObjectDefineProperties(proto, {
      [SymbolToStringTag]: {
        __proto__: null,
        value: 'QuotaExceededError',
        configurable: true,
      },
    });
  }
  return QuotaExceededError;
}

const kUsingTsImpl = autogates['ts-dom-exception'] === true;

const kDOMException: typeof DOMException = kUsingTsImpl
  ? install()
  : (globalThis as unknown as { DOMException: typeof DOMException })
      .DOMException;

export type DomExceptionModule = {
  DOMException: typeof DOMException;
  // Only under the autogate.
  QuotaExceededError: typeof QuotaExceededError | undefined;
};

module.exports = ObjectFreeze({
  DOMException: kDOMException,
  QuotaExceededError: kUsingTsImpl ? installQuotaExceededError() : undefined,
});
