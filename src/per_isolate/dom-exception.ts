'use strict';

// The DOMException the bootstrap's modules create and recognize, and that
// main.ts installs as the global under the TS_DOM_EXCEPTION autogate: the
// class below (WebIDL §3.14.1, §4.3). Without the autogate it is the C++
// class (jsg/dom-exception.h) that the global scope installs, captured as
// this module loads, before user code can replace the global. main.ts loads
// it before any other module that may use it.
//
// An instance is an API wrapper object (jsg::getBootstrapDomExceptionBase())
// with V8's stack, whose prototype is DOMException.prototype; Error.isError()
// reports it as a native error, as WebIDL requires. Its name and message live
// under two V8 private symbols, and the prototype carries a third, which
// native code also uses (jsg/dom-exception.h): Lock::domException() creates
// instances without running JavaScript, and the serializer and tryUnwrap()
// recognize them by the name, which is also the brand the attributes check.
// Native code finds the class through this module's exports.

const {
  Error: ErrorCtor,
  ErrorCaptureStackTrace,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectSetPrototypeOf,
  ReflectConstruct,
  SafeMap,
  SymbolFor,
  SymbolToStringTag,
  TypeError: TypeErrorCtor,
} = primordials;

const { getPrivateSymbol, DOMExceptionBase } = utils;

const kName = getPrivateSymbol('workerd:domExceptionName');
const kMessage = getPrivateSymbol('workerd:domExceptionMessage');
const kPrototype = getPrivateSymbol('workerd:domExceptionPrototype');

type Slots = Record<symbol, string | undefined>;

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
    (value as Slots)[kName] === undefined
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

const kDOMException: typeof DOMException = autogates['ts-dom-exception']
  ? install()
  : (globalThis as unknown as { DOMException: typeof DOMException })
      .DOMException;

export type DomExceptionModule = {
  DOMException: typeof DOMException;
};

module.exports = ObjectFreeze({ DOMException: kDOMException });
