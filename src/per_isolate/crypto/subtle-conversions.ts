'use strict';

// The conversions JSG applies to SubtleCrypto's arguments: the dictionaries
// (the JSG_STRUCTs of SubtleCrypto in api/crypto/crypto.h) and the types of
// their members. Each result holds only converted values, which the backend's
// operations convert again without running user code.
//
// What JSG does, which this follows rather than Web IDL:
//
// - A dictionary's members are read with [[Get]], prototype chain included,
//   in the C++ struct's declaration order, not lexicographically.
// - A dictionary from undefined or null throws "Cannot initialize <the C++
//   type's mangled name> ...", unless all its members are optional.
// - In `string or <dictionary>`, any object (a function or an array
//   included) is the dictionary, and any other value is converted to a
//   string.
// - An int is ToNumber, a non-finite number becomes 0, and the truncated
//   value must fit in 32 bits. A double is ToNumber; a boolean, ToBoolean.
// - A sequence must be an array (not a proxy for one); its elements are read
//   up to its length, holes from the prototype chain.
//
// crypto-subtle-parity-test.js runs every member through each unusual value
// against both implementations.

const {
  MathTrunc,
  NumberIsFinite,
  ObjectSetPrototypeOf,
  SafeWeakSet,
  TypeError,
} = primordials;

// As JSG unwraps jsg::JsBufferSource.
function isBufferSource(value: unknown): boolean {
  return utils.isArrayBuffer(value) || utils.isArrayBufferView(value);
}

type Dictionary = Record<string, unknown>;

// The errors these conversions create, which the caller gives the stack the
// C++ ones have. An error thrown by user code (a getter, a toString()) is not
// among them.
const ownErrors = new SafeWeakSet() as WeakSet<object>;

function ownTypeError(message: string): TypeError {
  const error = new TypeError(message);
  ownErrors.add(error);
  return error;
}

function isOwnError(error: unknown): boolean {
  return typeof error === 'object' && error !== null && ownErrors.has(error);
}

function memberError(
  dictionary: string,
  member: string,
  type: string
): TypeError {
  return ownTypeError(
    `Incorrect type for the '${member}' field on '${dictionary}': ` +
      `the provided value is not of type '${type}'.`
  );
}

function elementError(index: number, type: string): TypeError {
  return ownTypeError(
    `Incorrect type for array element ${index}: ` +
      `the provided value is not of type '${type}'.`
  );
}

// ToString, which throws for a Symbol.
function toStringValue(value: unknown): string {
  if (typeof value === 'string') return value;
  if (typeof value === 'symbol') {
    throw ownTypeError('Cannot convert a Symbol value to a string');
  }
  return `${value as string}`;
}

// ToNumber, which throws for a Symbol or a BigInt.
function toNumberValue(value: unknown): number {
  if (typeof value === 'number') return value;
  if (typeof value === 'symbol') {
    throw ownTypeError('Cannot convert a Symbol value to a number');
  }
  if (typeof value === 'bigint') {
    throw ownTypeError('Cannot convert a BigInt value to a number');
  }
  return +(value as number);
}

function toInt(value: unknown): number {
  const number = toNumberValue(value);
  if (!NumberIsFinite(number)) return 0;
  const truncated = MathTrunc(number);
  if (truncated > 2147483647 || truncated < -2147483648) {
    throw ownTypeError(
      'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).'
    );
  }
  return truncated;
}

// The kinds of member, by their C++ types.
const STRING = 0; // kj::String
const INT = 1; // int
const DOUBLE = 2; // double
const BOOLEAN = 3; // bool
const BUFFER_SOURCE = 4; // jsg::JsBufferSource
const CRYPTO_KEY = 5; // jsg::Ref<CryptoKey>
const ANY = 6; // jsg::JsValue
const HASH = 7; // kj::OneOf<kj::String, HashAlgorithm>
const STRINGS = 8; // kj::Array<kj::String>
const OTHER_PRIMES = 9; // kj::Array<RsaOtherPrimesInfo>
type Kind = number;

export type DictionaryType = {
  // As JSG names the struct in member errors.
  readonly name: string;
  // Name, kind and whether required, in declaration order.
  readonly members: readonly (readonly [string, Kind, boolean])[];
  // The result from each member's converted value, by its index. A literal
  // with every member gives each result one fast shape, and as each member is
  // an own property, the backend never reads the result's prototype.
  readonly create: (member: (index: number) => unknown) => Dictionary;
};

const HASH_ALGORITHM: DictionaryType = {
  name: 'HashAlgorithm',
  members: [['name', STRING, true]],
  create: (m) => ({
    name: m(0),
  }),
};
const ENCRYPT_ALGORITHM: DictionaryType = {
  name: 'EncryptAlgorithm',
  members: [
    ['name', STRING, true],
    ['iv', BUFFER_SOURCE, false],
    ['additionalData', BUFFER_SOURCE, false],
    ['tagLength', DOUBLE, false],
    ['counter', BUFFER_SOURCE, false],
    ['length', INT, false],
    ['label', BUFFER_SOURCE, false],
  ],
  create: (m) => ({
    name: m(0),
    iv: m(1),
    additionalData: m(2),
    tagLength: m(3),
    counter: m(4),
    length: m(5),
    label: m(6),
  }),
};
const SIGN_ALGORITHM: DictionaryType = {
  name: 'SignAlgorithm',
  members: [
    ['name', STRING, true],
    ['hash', HASH, false],
    ['dataLength', INT, false],
    ['saltLength', INT, false],
    ['context', ANY, false],
  ],
  create: (m) => ({
    name: m(0),
    hash: m(1),
    dataLength: m(2),
    saltLength: m(3),
    context: m(4),
  }),
};
const GENERATE_KEY_ALGORITHM: DictionaryType = {
  name: 'GenerateKeyAlgorithm',
  members: [
    ['name', STRING, true],
    ['hash', HASH, false],
    ['modulusLength', INT, false],
    ['publicExponent', BUFFER_SOURCE, false],
    ['length', INT, false],
    ['namedCurve', STRING, false],
  ],
  create: (m) => ({
    name: m(0),
    hash: m(1),
    modulusLength: m(2),
    publicExponent: m(3),
    length: m(4),
    namedCurve: m(5),
  }),
};
const IMPORT_KEY_ALGORITHM: DictionaryType = {
  name: 'ImportKeyAlgorithm',
  members: [
    ['name', STRING, true],
    ['hash', HASH, false],
    ['length', INT, false],
    ['namedCurve', STRING, false],
    ['compressed', BOOLEAN, false],
  ],
  create: (m) => ({
    name: m(0),
    hash: m(1),
    length: m(2),
    namedCurve: m(3),
    compressed: m(4),
  }),
};
const DERIVE_KEY_ALGORITHM: DictionaryType = {
  name: 'DeriveKeyAlgorithm',
  members: [
    ['name', STRING, true],
    ['salt', BUFFER_SOURCE, false],
    ['iterations', INT, false],
    ['hash', HASH, false],
    ['public', CRYPTO_KEY, false],
    ['info', BUFFER_SOURCE, false],
  ],
  create: (m) => ({
    name: m(0),
    salt: m(1),
    iterations: m(2),
    hash: m(3),
    public: m(4),
    info: m(5),
  }),
};
const RSA_OTHER_PRIMES_INFO: DictionaryType = {
  name: 'RsaOtherPrimesInfo',
  members: [
    ['r', STRING, false],
    ['d', STRING, false],
    ['t', STRING, false],
  ],
  create: (m) => ({
    r: m(0),
    d: m(1),
    t: m(2),
  }),
};
const JSON_WEB_KEY: DictionaryType = {
  name: 'JsonWebKey',
  members: [
    ['kty', STRING, true],
    ['use', STRING, false],
    ['key_ops', STRINGS, false],
    ['alg', STRING, false],
    ['ext', BOOLEAN, false],
    ['crv', STRING, false],
    ['x', STRING, false],
    ['y', STRING, false],
    ['d', STRING, false],
    ['n', STRING, false],
    ['e', STRING, false],
    ['p', STRING, false],
    ['q', STRING, false],
    ['dp', STRING, false],
    ['dq', STRING, false],
    ['qi', STRING, false],
    ['oth', OTHER_PRIMES, false],
    ['k', STRING, false],
    ['pub', STRING, false],
    ['priv', STRING, false],
  ],
  create: (m) => ({
    kty: m(0),
    use: m(1),
    key_ops: m(2),
    alg: m(3),
    ext: m(4),
    crv: m(5),
    x: m(6),
    y: m(7),
    d: m(8),
    n: m(9),
    e: m(10),
    p: m(11),
    q: m(12),
    dp: m(13),
    dq: m(14),
    qi: m(15),
    oth: m(16),
    k: m(17),
    pub: m(18),
    priv: m(19),
  }),
};

function absent(): undefined {
  return undefined;
}

// What only native code knows. `native` returns it, created on first use.
export interface ConversionNatives {
  isCryptoKey(value: unknown): boolean;
  // typeid(T).name() of the struct `name`, which JSG quotes and which depends
  // on the C++ ABI.
  structTypeName(name: string): string;
}

function hasRequiredMember(type: DictionaryType): boolean {
  const members = type.members;
  for (let i = 0; i < members.length; i++) {
    if (members[i]![2]) return true;
  }
  return false;
}

function createConversions(native: () => ConversionNatives) {
  // A sequence of `element`s, which throws `notArray` for a value that is not
  // an array.
  function toSequence(
    value: unknown,
    element: (element: unknown, index: number) => unknown,
    notArray: () => TypeError
  ): unknown[] {
    if (!utils.isArray(value)) throw notArray();
    const array = value as unknown[];
    const length = array.length;
    // Null-prototype, so that no setter on Array.prototype runs.
    const result = ObjectSetPrototypeOf([], null) as unknown[];
    for (let i = 0; i < length; i++) {
      result[i] = element(array[i], i);
    }
    return result;
  }

  function toMember(
    dictionary: string,
    member: string,
    kind: Kind,
    value: unknown
  ): unknown {
    switch (kind) {
      case STRING:
        return toStringValue(value);
      case INT:
        return toInt(value);
      case DOUBLE:
        return toNumberValue(value);
      case BOOLEAN:
        return !!value;
      case BUFFER_SOURCE:
        if (!isBufferSource(value)) {
          throw memberError(dictionary, member, 'BufferSource');
        }
        return value;
      case CRYPTO_KEY:
        if (!native().isCryptoKey(value)) {
          throw memberError(dictionary, member, 'CryptoKey');
        }
        return value;
      case ANY:
        return value;
      case HASH:
        return toAlgorithm(HASH_ALGORITHM, value);
      case STRINGS:
        return toSequence(value, toStringValue, () =>
          memberError(dictionary, member, 'Array')
        );
      default:
        return toSequence(
          value,
          (element, index) => {
            const info = toDictionary(RSA_OTHER_PRIMES_INFO, element);
            if (info === null) {
              throw elementError(index, RSA_OTHER_PRIMES_INFO.name);
            }
            return info;
          },
          () => memberError(dictionary, member, 'Array')
        );
    }
  }

  // The dictionary, or null for a value that is not an object.
  function toDictionary(
    type: DictionaryType,
    value: unknown
  ): Dictionary | null {
    if (value === undefined || value === null) {
      if (!hasRequiredMember(type)) return type.create(absent);
      throw ownTypeError(
        `Cannot initialize ${native().structTypeName(type.name)} with ` +
          'required members from ' +
          'an undefined or null value.'
      );
    }
    if (typeof value !== 'object' && typeof value !== 'function') return null;
    const members = type.members;
    return type.create((index) => {
      const entry = members[index]!;
      const member = entry[0];
      const memberValue = (value as Dictionary)[member];
      // An absent optional member stays undefined, as kj::none.
      if (memberValue === undefined && !entry[2]) return undefined;
      return toMember(type.name, member, entry[1], memberValue);
    });
  }

  // `string or <dictionary>`: the dictionary, given a string as its name, the
  // first member of each algorithm dictionary.
  function toAlgorithm(type: DictionaryType, value: unknown): Dictionary {
    const dictionary = toDictionary(type, value);
    if (dictionary !== null) return dictionary;
    const name = toStringValue(value);
    return type.create((index) => (index === 0 ? name : undefined));
  }

  return {
    toAlgorithm,
    toDictionary,
    toInt,
    toStrings(value: unknown, notArray: () => TypeError): unknown[] {
      return toSequence(value, toStringValue, notArray);
    },
  };
}

export type SubtleConversions = {
  ENCRYPT_ALGORITHM: DictionaryType;
  SIGN_ALGORITHM: DictionaryType;
  HASH_ALGORITHM: DictionaryType;
  GENERATE_KEY_ALGORITHM: DictionaryType;
  IMPORT_KEY_ALGORITHM: DictionaryType;
  DERIVE_KEY_ALGORITHM: DictionaryType;
  JSON_WEB_KEY: DictionaryType;
  createConversions: typeof createConversions;
  isBufferSource: typeof isBufferSource;
  isOwnError: typeof isOwnError;
  ownTypeError: typeof ownTypeError;
  toStringValue: typeof toStringValue;
};

module.exports = {
  ENCRYPT_ALGORITHM,
  SIGN_ALGORITHM,
  HASH_ALGORITHM,
  GENERATE_KEY_ALGORITHM,
  IMPORT_KEY_ALGORITHM,
  DERIVE_KEY_ALGORITHM,
  JSON_WEB_KEY,
  createConversions,
  isBufferSource,
  isOwnError,
  ownTypeError,
  toStringValue,
};
