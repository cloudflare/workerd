'use strict';

// SubtleCrypto, under the TS_WEBCRYPTO autogate: the interface over the native
// `cryptoBackend` (a C++ SubtleCrypto, api/crypto/crypto.h), which replaces the C++ class as
// the global SubtleCrypto and as `crypto.subtle`. See crypto/AGENTS.md; the
// parity suite (src/tests/crypto/crypto-subtle-parity-test.js) pins it to the C++
// class.

import type { RuntimeClassHelpers } from '../runtime-class';
import type { SerializationHelpers } from '../serialization';
import type { CommonHelpers } from '../webstreams/common';
import type { DictionaryType, SubtleConversions } from './subtle-conversions';

const {
  ObjectCreate,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectPrototype,
  PromisePrototypeThen,
  PromiseReject,
  PromiseResolve,
  TypeError,
} = primordials;

const { markAsRuntimeClass, setNameAndLength, unwrapReceiver } =
  require('runtime-class') as RuntimeClassHelpers;
const { markUncloneable } = require('serialization') as SerializationHelpers;
const { kNullProtoObject } = require('webstreams/common') as Pick<
  CommonHelpers,
  'kNullProtoObject'
>;
const {
  ENCRYPT_ALGORITHM,
  SIGN_ALGORITHM,
  HASH_ALGORITHM,
  GENERATE_KEY_ALGORITHM,
  IMPORT_KEY_ALGORITHM,
  DERIVE_KEY_ALGORITHM,
  JSON_WEB_KEY,
  createConversions,
  toStringValue,
} = require('crypto/subtle-conversions') as SubtleConversions;

const conversions = createConversions(cryptoBackend);

const kCaptureThrows = compatFlags['capture_async_api_throws'] === true;
const kModernAlgorithms = compatFlags['webcrypto_modern_algorithms'] === true;
const kSpecCompliantPropertyAttributes =
  compatFlags['spec_compliant_property_attributes'] === true;
const kImmutablePrototypes = compatFlags['immutable_api_prototypes'] === true;

// Each member's required parameters, as JSG counts them for its length.
const kRequiredArguments: Record<string, number> = {
  __proto__: kNullProtoObject,
  encrypt: 3,
  decrypt: 3,
  sign: 3,
  verify: 4,
  digest: 2,
  generateKey: 3,
  deriveKey: 5,
  deriveBits: 2,
  importKey: 5,
  exportKey: 2,
  wrapKey: 4,
  unwrapKey: 7,
  encapsulateKey: 5,
  encapsulateBits: 2,
  decapsulateKey: 6,
  decapsulateBits: 3,
  getPublicKey: 2,
  timingSafeEqual: 2,
  supports: 2,
} as never;

// V8-private, so the brand can be neither read nor forged by user code.
const kBrand = utils.getPrivateSymbol('workerd:SubtleCryptoBrand');

function checkReceiver(self: unknown): void {
  unwrapReceiver(self, kBrand);
}

function notOfType(method: string, index: number, type: string): TypeError {
  return new TypeError(
    `Failed to execute '${method}' on 'SubtleCrypto': ` +
      `parameter ${index + 1} is not of type '${type}'.`
  );
}

// The argument at `index`, which JSG requires to have been passed even where
// its type accepts undefined.
function required(
  method: string,
  args: unknown[],
  index: number,
  type: string
): unknown {
  if (index >= args.length) throw notOfType(method, index, type);
  return args[index];
}

type Converter = (method: string, args: unknown[], index: number) => unknown;

// A converter for a parameter that the value passes through unchanged once
// `test` accepts it.
function checked(type: string, test: (value: unknown) => boolean): Converter {
  return (method, args, index) => {
    const value = required(method, args, index, type);
    if (!test(value)) throw notOfType(method, index, type);
    return value;
  };
}

const cryptoKey = checked('CryptoKey', (v) => cryptoBackend.isCryptoKey(v));
const bufferSource = checked('BufferSource', utils.isBufferSource);
// As JSG unwraps kj::Array<const kj::byte>.
const bytes = checked(
  'Array',
  (v) => utils.isBufferSource(v) || utils.isAnyArrayBuffer(v)
);

// `string or <dictionary>` parameters.
function algorithm(type: DictionaryType): Converter {
  const typeName = `string or ${type.name}`;
  return (method, args, index) =>
    conversions.toAlgorithm(type, required(method, args, index, typeName));
}
const encryptAlgorithm = algorithm(ENCRYPT_ALGORITHM);
const signAlgorithm = algorithm(SIGN_ALGORITHM);
const hashAlgorithm = algorithm(HASH_ALGORITHM);
const generateKeyAlgorithm = algorithm(GENERATE_KEY_ALGORITHM);
const importKeyAlgorithm = algorithm(IMPORT_KEY_ALGORITHM);
const deriveKeyAlgorithm = algorithm(DERIVE_KEY_ALGORITHM);

const keyUsages: Converter = (method, args, index) =>
  conversions.toStrings(required(method, args, index, 'Array'), () =>
    notOfType(method, index, 'Array')
  );

// `BufferSource or JsonWebKey`.
const importKeyData: Converter = (method, args, index) => {
  const type = 'BufferSource or JsonWebKey';
  const value = required(method, args, index, type);
  if (utils.isBufferSource(value)) return value;
  const jwk = conversions.toDictionary(JSON_WEB_KEY, value);
  if (jwk === null) throw notOfType(method, index, type);
  return jwk;
};

// deriveBits()'s optional length, null for none, checked before the
// operation begins.
function deriveBitsLength(value: unknown): number | null {
  if (value === undefined || value === null) return null;
  const length = conversions.toInt(value);
  if (length < 0) {
    throw new TypeError('deriveBits length must be an unsigned long integer.');
  }
  return length;
}

// JSG's kj::String conversion, ToString, under which a Symbol throws.
const string: Converter = (method, args, index) =>
  toStringValue(required(method, args, index, 'string'));
const boolean: Converter = (method, args, index) =>
  !!required(method, args, index, 'boolean');

// Runs a method: `convert` converts the arguments and returns the operation.
// The operation runs now, as the C++ class runs it, so buffers are read at the
// call; its promise settles a microtask later than a resolved promise, as the
// C++ one does, which the extra then() reproduces. A failed conversion rejects
// without it, as under the C++ class.
function invoke(convert: () => () => unknown): Promise<unknown> {
  let operation: () => unknown;
  try {
    operation = convert();
  } catch (error) {
    if (!kCaptureThrows) throw error;
    return PromiseReject(error);
  }
  let result: Promise<unknown>;
  try {
    result = PromiseResolve(operation());
  } catch (error) {
    result = PromiseReject(error);
  }
  return PromisePrototypeThen(result, undefined);
}

// Method shorthand, so that each member is unconstructible and, with the rest
// parameter, has length 0, as a JSG method does.
const members = {
  encrypt(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = encryptAlgorithm('encrypt', args, 0);
      const key = cryptoKey('encrypt', args, 1);
      const data = bufferSource('encrypt', args, 2);
      return () => cryptoBackend.encrypt(algorithm, key, data);
    });
  },
  decrypt(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = encryptAlgorithm('decrypt', args, 0);
      const key = cryptoKey('decrypt', args, 1);
      const data = bufferSource('decrypt', args, 2);
      return () => cryptoBackend.decrypt(algorithm, key, data);
    });
  },
  sign(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = signAlgorithm('sign', args, 0);
      const key = cryptoKey('sign', args, 1);
      const data = bufferSource('sign', args, 2);
      return () => cryptoBackend.sign(algorithm, key, data);
    });
  },
  verify(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = signAlgorithm('verify', args, 0);
      const key = cryptoKey('verify', args, 1);
      const signature = bufferSource('verify', args, 2);
      const data = bufferSource('verify', args, 3);
      return () => cryptoBackend.verify(algorithm, key, signature, data);
    });
  },
  digest(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = hashAlgorithm('digest', args, 0);
      const data = bufferSource('digest', args, 1);
      return () => cryptoBackend.digest(algorithm, data);
    });
  },
  generateKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = generateKeyAlgorithm('generateKey', args, 0);
      const extractable = boolean('generateKey', args, 1);
      const usages = keyUsages('generateKey', args, 2);
      return () => cryptoBackend.generateKey(algorithm, extractable, usages);
    });
  },
  deriveKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = deriveKeyAlgorithm('deriveKey', args, 0);
      const baseKey = cryptoKey('deriveKey', args, 1);
      const derivedKeyAlgorithm = importKeyAlgorithm('deriveKey', args, 2);
      const extractable = boolean('deriveKey', args, 3);
      const usages = keyUsages('deriveKey', args, 4);
      return () =>
        cryptoBackend.deriveKey(
          algorithm,
          baseKey,
          derivedKeyAlgorithm,
          extractable,
          usages
        );
    });
  },
  deriveBits(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = deriveKeyAlgorithm('deriveBits', args, 0);
      const baseKey = cryptoKey('deriveBits', args, 1);
      const length = deriveBitsLength(args[2]);
      return () => cryptoBackend.deriveBits(algorithm, baseKey, length);
    });
  },
  importKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const format = string('importKey', args, 0);
      const keyData = importKeyData('importKey', args, 1);
      const algorithm = importKeyAlgorithm('importKey', args, 2);
      const extractable = boolean('importKey', args, 3);
      const usages = keyUsages('importKey', args, 4);
      return () =>
        cryptoBackend.importKey(
          format,
          keyData,
          algorithm,
          extractable,
          usages
        );
    });
  },
  exportKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const format = string('exportKey', args, 0);
      const key = cryptoKey('exportKey', args, 1);
      return () => cryptoBackend.exportKey(format, key);
    });
  },
  wrapKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const format = string('wrapKey', args, 0);
      const key = cryptoKey('wrapKey', args, 1);
      const wrappingKey = cryptoKey('wrapKey', args, 2);
      const algorithm = encryptAlgorithm('wrapKey', args, 3);
      return () => cryptoBackend.wrapKey(format, key, wrappingKey, algorithm);
    });
  },
  unwrapKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const format = string('unwrapKey', args, 0);
      const wrappedKey = bufferSource('unwrapKey', args, 1);
      const unwrappingKey = cryptoKey('unwrapKey', args, 2);
      const algorithm = encryptAlgorithm('unwrapKey', args, 3);
      const unwrappedKeyAlgorithm = importKeyAlgorithm('unwrapKey', args, 4);
      const extractable = boolean('unwrapKey', args, 5);
      const usages = keyUsages('unwrapKey', args, 6);
      return () =>
        cryptoBackend.unwrapKey(
          format,
          wrappedKey,
          unwrappingKey,
          algorithm,
          unwrappedKeyAlgorithm,
          extractable,
          usages
        );
    });
  },
};

// Only under webcrypto_modern_algorithms.
const modernMembers = {
  encapsulateKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = importKeyAlgorithm('encapsulateKey', args, 0);
      const key = cryptoKey('encapsulateKey', args, 1);
      const sharedKeyAlgorithm = importKeyAlgorithm('encapsulateKey', args, 2);
      const extractable = boolean('encapsulateKey', args, 3);
      const usages = keyUsages('encapsulateKey', args, 4);
      return () =>
        cryptoBackend.encapsulateKey(
          algorithm,
          key,
          sharedKeyAlgorithm,
          extractable,
          usages
        );
    });
  },
  encapsulateBits(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = importKeyAlgorithm('encapsulateBits', args, 0);
      const key = cryptoKey('encapsulateBits', args, 1);
      return () => cryptoBackend.encapsulateBits(algorithm, key);
    });
  },
  decapsulateKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = importKeyAlgorithm('decapsulateKey', args, 0);
      const key = cryptoKey('decapsulateKey', args, 1);
      const ciphertext = bytes('decapsulateKey', args, 2);
      const sharedKeyAlgorithm = importKeyAlgorithm('decapsulateKey', args, 3);
      const extractable = boolean('decapsulateKey', args, 4);
      const usages = keyUsages('decapsulateKey', args, 5);
      return () =>
        cryptoBackend.decapsulateKey(
          algorithm,
          key,
          ciphertext,
          sharedKeyAlgorithm,
          extractable,
          usages
        );
    });
  },
  decapsulateBits(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const algorithm = importKeyAlgorithm('decapsulateBits', args, 0);
      const key = cryptoKey('decapsulateBits', args, 1);
      const ciphertext = bytes('decapsulateBits', args, 2);
      return () => cryptoBackend.decapsulateBits(algorithm, key, ciphertext);
    });
  },
  getPublicKey(...args: unknown[]): Promise<unknown> {
    checkReceiver(this);
    return invoke(() => {
      const key = cryptoKey('getPublicKey', args, 0);
      const usages = keyUsages('getPublicKey', args, 1);
      return () => cryptoBackend.getPublicKey(key, usages);
    });
  },
};

// Non-standard, after Node.js' crypto.timingSafeEqual(). Synchronous.
const syncMembers = {
  timingSafeEqual(...args: unknown[]): boolean {
    checkReceiver(this);
    const a = bufferSource('timingSafeEqual', args, 0);
    const b = bufferSource('timingSafeEqual', args, 1);
    return cryptoBackend.timingSafeEqual(a, b);
  },
};

const staticMembers = {
  supports(...args: unknown[]): boolean {
    const operation = string('supports', args, 0) as string;
    const algorithm = required('supports', args, 1, 'Value');
    return cryptoBackend.supports(operation, algorithm, args[2]);
  },
};

// A function, not a class, so that calling it without `new` throws the same
// TypeError as constructing it.
function SubtleCrypto(): never {
  throw new TypeError('Illegal constructor');
}

// The descriptors of `source`'s members, which under
// spec_compliant_property_attributes are given lengths counting their required
// arguments.
function memberDescriptors(
  source: Record<string, Function>
): PropertyDescriptorMap {
  const descriptors: PropertyDescriptorMap = {
    __proto__: kNullProtoObject,
  } as never;
  for (const name in source) {
    const fn = source[name]!;
    if (kSpecCompliantPropertyAttributes) {
      setNameAndLength(fn, name, kRequiredArguments[name]!);
    }
    descriptors[name] = {
      __proto__: kNullProtoObject,
      value: fn,
      writable: true,
      enumerable: true,
      configurable: true,
    } as PropertyDescriptor;
  }
  return descriptors;
}

// Members in JSG's order: the methods, then the constructor.
const proto = ObjectCreate(ObjectPrototype) as object;
ObjectDefineProperties(proto, memberDescriptors(members));
if (kModernAlgorithms) {
  ObjectDefineProperties(proto, memberDescriptors(modernMembers));
}
ObjectDefineProperties(proto, memberDescriptors(syncMembers));
ObjectDefineProperty(proto, 'constructor', {
  __proto__: kNullProtoObject,
  value: SubtleCrypto,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);
// Writable, as a JSG class's is, unless immutable_api_prototypes.
ObjectDefineProperty(SubtleCrypto, 'prototype', {
  __proto__: kNullProtoObject,
  value: proto,
  writable: !kImmutablePrototypes,
} as PropertyDescriptor);
if (kModernAlgorithms) {
  ObjectDefineProperties(SubtleCrypto, memberDescriptors(staticMembers));
}
markAsRuntimeClass(proto, 'SubtleCrypto');

// The context's one instance, which `crypto.subtle` returns, created on first
// use. crypto.ts's getCrypto() calls this.
let instance: object | undefined;
function getSubtleCrypto(): object {
  if (instance === undefined) {
    const subtle = ObjectCreate(proto) as Record<symbol, unknown>;
    subtle[kBrand] = true;
    markUncloneable(subtle);
    instance = subtle;
  }
  return instance;
}

export type SubtleCryptoExports = {
  SubtleCrypto: typeof SubtleCrypto;
  getSubtleCrypto: typeof getSubtleCrypto;
};

module.exports = {
  SubtleCrypto,
  getSubtleCrypto,
};
