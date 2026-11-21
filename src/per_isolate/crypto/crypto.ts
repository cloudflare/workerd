'use strict';

// Crypto, under the TS_WEBCRYPTO autogate: the class of the `crypto` global,
// which replaces the C++ one (api/crypto/crypto.h). Randomness stays native:
// utils.fillRandomValues() fills a view from the request's entropy source, and
// utils.newRandomUUID() makes a UUID from it. See
// crypto/AGENTS.md; the parity suite (src/tests/crypto/crypto-global-parity-test.js)
// pins it to the C++ class.

import type { DomExceptionModule } from '../dom-exception';
import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';
import type { CommonHelpers } from '../webstreams/common';
import type { SubtleCryptoExports } from './subtle';

const {
  ObjectCreate,
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  ObjectPrototype,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetSymbolToStringTag,
  TypeError,
} = primordials;

const { markAsRuntimeClass, setNameAndLength, unwrapReceiver } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;
const { getSubtleCrypto } = require('crypto/subtle') as SubtleCryptoExports;
const { DOMException } = require('dom-exception') as DomExceptionModule;
const { kNullProtoObject } = require('webstreams/common') as Pick<
  CommonHelpers,
  'kNullProtoObject'
>;

const { fillRandomValues, isArrayBufferView, newRandomUUID } = utils;

const kSpecCompliantPropertyAttributes =
  compatFlags['spec_compliant_property_attributes'] === true;
const kImmutablePrototypes = compatFlags['immutable_api_prototypes'] === true;
// `subtle` is an own property of each instance unless
// workers_api_getters_setters_on_prototype.
const kInstanceProperties =
  compatFlags['workers_api_getters_setters_on_prototype'] !== true;

// V8-private, so the brand can be neither read nor forged by user code. Holds
// the instance's `subtle`.
const kSubtle = utils.getPrivateSymbol('workerd:CryptoSubtle');

// The typed arrays getRandomValues() fills, by [[TypedArrayName]].
const kIntegerTypes: Record<string, true> = {
  __proto__: kNullProtoObject,
  Int8Array: true,
  Uint8Array: true,
  Uint8ClampedArray: true,
  Int16Array: true,
  Uint16Array: true,
  Int32Array: true,
  Uint32Array: true,
  BigInt64Array: true,
  BigUint64Array: true,
} as never;

const kMaxRandomBytes = 0x10000;

function subtleOf(self: unknown): object {
  return unwrapReceiver<object>(self, kSubtle);
}

const members = {
  getRandomValues(buffer: unknown): unknown {
    subtleOf(this);
    const type = TypedArrayPrototypeGetSymbolToStringTag(buffer);
    if (type === undefined || kIntegerTypes[type] !== true) {
      if (!isArrayBufferView(buffer)) {
        throw new TypeError(
          "Failed to execute 'getRandomValues' on 'Crypto': " +
            "parameter 1 is not of type 'ArrayBufferView'."
        );
      }
      // TypeMismatchError is deprecated in favor of TypeError, but the spec
      // and the W3C tests still expect it here.
      throw new DOMException(
        'ArrayBufferView argument to getRandomValues() must be an ' +
          'integer-typed view.',
        'TypeMismatchError'
      );
    }
    const view = buffer as ArrayBufferView;
    const size = TypedArrayPrototypeGetByteLength(view);
    if (size > kMaxRandomBytes) {
      throw new DOMException(
        'getRandomValues() only accepts buffers of size <= 64K but ' +
          `provided ${size} bytes.`,
        'QuotaExceededError'
      );
    }
    fillRandomValues(view);
    return view;
  },
  // Formatted natively: building the string in JavaScript costs about as much
  // as the whole C++ call.
  randomUUID(): string {
    subtleOf(this);
    return newRandomUUID();
  },
};

// The `subtle` getter defineCrypto() installs on the prototype, in place of
// JSG's; defineCrypto() gives it JSG's name and length.
const subtleGetter = {
  get subtle(): object {
    return subtleOf(this);
  },
};
const subtleGetterFunction = ObjectGetOwnPropertyDescriptor(
  subtleGetter,
  'subtle'
)!.get!;

// A function, not a class, so that calling it without `new` throws the same
// TypeError as constructing it.
function Crypto(): never {
  throw new TypeError('Illegal constructor');
}

const proto = ObjectCreate(ObjectPrototype) as object;

// Finishes the class, in JSG's order: `subtle` (on the prototype, unless it is
// an instance property), the methods, `DigestStream`, the constructor.
function defineCrypto(DigestStream: unknown): typeof Crypto {
  if (!kInstanceProperties) {
    if (kSpecCompliantPropertyAttributes) {
      setNameAndLength(subtleGetterFunction, 'get subtle', 0);
    } else {
      setNameAndLength(subtleGetterFunction, '', 0);
    }
    ObjectDefineProperties(proto, {
      __proto__: kNullProtoObject,
      subtle: {
        __proto__: kNullProtoObject,
        get: subtleGetterFunction,
        enumerable: true,
        configurable: true,
      },
    });
  }
  setNameAndLength(
    members.getRandomValues,
    'getRandomValues',
    kSpecCompliantPropertyAttributes ? 1 : 0
  );
  ObjectDefineProperties(proto, {
    __proto__: kNullProtoObject,
    getRandomValues: {
      __proto__: kNullProtoObject,
      value: members.getRandomValues,
      writable: true,
      enumerable: true,
      configurable: true,
    },
    randomUUID: {
      __proto__: kNullProtoObject,
      value: members.randomUUID,
      writable: true,
      enumerable: true,
      configurable: true,
    },
    DigestStream: {
      __proto__: kNullProtoObject,
      value: DigestStream,
      writable: true,
      enumerable: true,
      configurable: true,
    },
    constructor: {
      __proto__: kNullProtoObject,
      value: Crypto,
      writable: true,
      enumerable: false,
      configurable: true,
    },
  });
  // Writable, as a JSG class's is, unless immutable_api_prototypes.
  ObjectDefineProperties(Crypto, {
    __proto__: kNullProtoObject,
    prototype: {
      __proto__: kNullProtoObject,
      value: proto,
      writable: !kImmutablePrototypes,
    },
  });
  markAsRuntimeClass(proto, 'Crypto');
  return Crypto;
}

// The DigestStream is the TypeScript one under typescript_implemented_streams,
// and otherwise the C++ class, which the backend exports.
defineCrypto(
  compatFlags['typescript_implemented_streams']
    ? (require('crypto/digest-stream') as { DigestStream: unknown })
        .DigestStream
    : cryptoBackend.DigestStream
);

// The context's one instance, the `crypto` global, created on first use.
// ServiceWorkerGlobalScope::getCrypto() (api/global-scope.h) calls this,
// through crypto/cpp_exports.
let instance: object | undefined;
function getCrypto(): object {
  instance ??= newCrypto(getSubtleCrypto());
  return instance;
}

function newCrypto(subtle: object): object {
  const crypto = ObjectCreate(proto) as Record<symbol, unknown>;
  crypto[kSubtle] = subtle;
  if (kInstanceProperties) {
    // JSG's read-only instance property, which reads the same `subtle` each
    // time.
    ObjectDefineProperties(crypto, {
      __proto__: kNullProtoObject,
      subtle: {
        __proto__: kNullProtoObject,
        value: subtle,
        writable: false,
        enumerable: true,
        configurable: true,
      },
    });
  }
  markUncloneable(crypto);
  return crypto;
}

export type CryptoExports = {
  Crypto: typeof Crypto;
  getCrypto: typeof getCrypto;
};

module.exports = {
  Crypto,
  getCrypto,
};
