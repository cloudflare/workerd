// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A worker of its own, whose top level replaces built-ins before the first
// read of `Navigator` and of `navigator`, which is what loads the TypeScript
// implementation and makes the instance; and, as nothing else runs in this
// worker, the changes to the instance that cannot be undone.
//
// While built-ins are replaced, this module uses only what it captured
// first, syntax, and index loops.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { env } from 'cloudflare:workers';

const flags = globalThis.Cloudflare.compatibilityFlags;
const hasLanguage = flags['enable_navigator_language'] === true;
const hasStorage = flags['enable_web_file_system'] === true;

const { defineProperty, getOwnPropertyDescriptor, getPrototypeOf } = Object;
const { deleteProperty, ownKeys } = Reflect;

// What kind of property each global of the TypeScript implementation is
// before anything has read it. (Getting the descriptor of a C++ one reads
// it.)
function kindOf(object, key) {
  const descriptor = getOwnPropertyDescriptor(object, key);
  if (descriptor === undefined) return 'none';
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}
const kUnreadKinds =
  env.USING_TS_IMPL === true
    ? [
        kindOf(globalThis, 'navigator'),
        kindOf(getPrototypeOf(globalThis), 'Navigator'),
        kindOf(globalThis, 'Navigator'),
      ]
    : null;

const kCalled = new Error('a replaced built-in was called');
function fail() {
  throw kCalled;
}

// The replaced Object.defineProperty: records its calls, and defines
// nothing. While `breakLoading` it also reads `navigator` and throws.
const definePropertyCalls = [];
let breakLoading = true;
let reentrantRead = 'not read';
function recordDefineProperty(target, name, descriptor) {
  // Not an assignment, which would reach the accessor on Object.prototype.
  defineProperty(definePropertyCalls, definePropertyCalls.length, {
    __proto__: null,
    value: { __proto__: null, target, name, descriptor },
    writable: true,
    enumerable: true,
    configurable: true,
  });
  if (breakLoading) {
    try {
      reentrantRead = globalThis.navigator;
    } catch (error) {
      reentrantRead = error;
    }
    throw kCalled;
  }
  return target;
}

const kStatics = [
  [Object, 'create'],
  [Object, 'defineProperty'],
  [Object, 'defineProperties'],
  [Object, 'setPrototypeOf'],
  [Object, 'freeze'],
  [Array, 'of'],
  [Array, 'from'],
  [Reflect, 'defineProperty'],
  [Function.prototype, 'call'],
  [Function.prototype, 'apply'],
  [Array.prototype, 'push'],
];
const kAccessors = [
  'value',
  'writable',
  'enumerable',
  'configurable',
  'get',
  'set',
  'storage',
  'languages',
  'language',
  'platform',
  'hardwareConcurrency',
  'userAgent',
  '0',
];

let accessorRan = false;
const savedStatics = [];
for (let i = 0; i < kStatics.length; i++) {
  savedStatics[i] = getOwnPropertyDescriptor(kStatics[i][0], kStatics[i][1]);
  defineProperty(kStatics[i][0], kStatics[i][1], {
    __proto__: null,
    value:
      kStatics[i][1] === 'defineProperty' && kStatics[i][0] === Object
        ? recordDefineProperty
        : fail,
    writable: true,
    configurable: true,
  });
}
for (let i = 0; i < kAccessors.length; i++) {
  defineProperty(Object.prototype, kAccessors[i], {
    __proto__: null,
    get() {
      accessorRan = true;
      return undefined;
    },
    set() {
      accessorRan = true;
    },
    configurable: true,
  });
}

// The first read of `Navigator`, while the replaced Object.defineProperty
// throws; then it no longer does.
let brokenRead;
let brokenReadError;
try {
  brokenRead = Navigator;
} catch (error) {
  brokenReadError = error;
}
breakLoading = false;

// What user code assigns to Navigator.prototype, where it can, before the
// instance is made.
const kPrototype = Navigator.prototype;
const kReplacement = { replaced: true };
let replaced = true;
try {
  Navigator.prototype = kReplacement;
} catch {
  // Read-only under immutable_api_prototypes.
  replaced = false;
}

let firstRead;
let firstReadError;
let firstKeys;
let firstLanguages;
try {
  firstRead = navigator;
  firstKeys = ownKeys(firstRead);
  if (hasLanguage) firstLanguages = firstRead.languages;
} catch (error) {
  firstReadError = error;
} finally {
  try {
    Navigator.prototype = kPrototype;
  } catch {
    // Never replaced.
  }
  for (let i = kAccessors.length - 1; i >= 0; i--) {
    deleteProperty(Object.prototype, kAccessors[i]);
  }
  for (let i = kStatics.length - 1; i >= 0; i--) {
    defineProperty(kStatics[i][0], kStatics[i][1], savedStatics[i]);
  }
}

const kExpectedKeys = [
  ...(hasStorage ? ['storage'] : []),
  ...(hasLanguage ? ['languages', 'language'] : []),
  'platform',
  'hardwareConcurrency',
  'userAgent',
];

function attributesOf(object, key) {
  const descriptor = getOwnPropertyDescriptor(object, key);
  if (descriptor === undefined) return 'none';
  return [
    'value' in descriptor ? 'data' : 'accessor',
    descriptor.writable ? 'w' : '',
    descriptor.enumerable ? 'e' : '',
    descriptor.configurable ? 'c' : '',
  ].join('');
}

export default {
  test() {
    const usingTsImpl = env.USING_TS_IMPL === true;
    const { sendBeacon } = kPrototype;

    // Ledger: until it is read, a TypeScript global is an accessor with the
    // enumerability of the data property that it then becomes, and that a
    // C++ one is from the start.
    const specCompliant = flags['spec_compliant_property_attributes'] === true;
    if (usingTsImpl) {
      deepStrictEqual(kUnreadKinds, [
        'accessorec',
        'accessorec',
        specCompliant ? 'accessorc' : 'none',
      ]);
    }
    strictEqual(kindOf(globalThis, 'navigator'), 'datawec');
    strictEqual(kindOf(getPrototypeOf(globalThis), 'Navigator'), 'datawec');
    strictEqual(
      kindOf(globalThis, 'Navigator'),
      specCompliant ? 'datawc' : 'none'
    );

    if (usingTsImpl) {
      // Ledger: the module loads at the first read of either global, and the
      // code TypeScript compiles it to begins by calling the global
      // Object.defineProperty on its `exports` object. User code that
      // replaced it is called with that object, which stays empty and which
      // nothing else uses, and what it throws fails the read; the next read
      // loads the module again. A read of `navigator` from inside it throws.
      strictEqual(brokenRead, undefined);
      strictEqual(brokenReadError, kCalled);
      strictEqual(reentrantRead instanceof TypeError, true);
      strictEqual(
        reentrantRead.message,
        'The navigator global is being initialized.'
      );
      strictEqual(definePropertyCalls.length >= 2, true);
      for (const { target, name, descriptor } of definePropertyCalls) {
        strictEqual(name, '__esModule');
        deepStrictEqual(ownKeys(target), []);
        strictEqual(getPrototypeOf(target), Object.prototype);
        deepStrictEqual(ownKeys(descriptor), ['value']);
        strictEqual(descriptor.value, true);
      }
    } else {
      strictEqual(brokenRead, Navigator);
      strictEqual(brokenReadError, undefined);
      strictEqual(reentrantRead, 'not read');
      strictEqual(definePropertyCalls.length, 0);
    }

    // The first read of `navigator`, among replaced built-ins.
    strictEqual(firstReadError, undefined);
    strictEqual(accessorRan, false);
    strictEqual(firstRead, navigator);
    strictEqual(Navigator.prototype, kPrototype);
    // Ledger: V8 makes a C++ instance with the object that the
    // constructor's `prototype` is at the time; the TypeScript one is made
    // with the prototype the constructor began with.
    const inheritsReplacement = replaced && !usingTsImpl;
    strictEqual(
      getPrototypeOf(firstRead),
      inheritsReplacement ? kReplacement : kPrototype
    );
    strictEqual(firstRead.replaced, inheritsReplacement ? true : undefined);
    strictEqual(firstRead instanceof Navigator, !inheritsReplacement);
    strictEqual(
      firstRead.sendBeacon,
      inheritsReplacement ? undefined : sendBeacon
    );
    deepStrictEqual(firstKeys, kExpectedKeys);
    for (const key of kExpectedKeys) {
      strictEqual(attributesOf(navigator, key), 'dataec', key);
    }
    strictEqual(navigator.userAgent, 'Cloudflare-Workers');
    strictEqual(navigator.hardwareConcurrency, 1);
    strictEqual(navigator.platform, '');
    if (hasLanguage) {
      strictEqual(navigator.language, 'en');
      deepStrictEqual(firstLanguages, ['en']);
      strictEqual(getPrototypeOf(firstLanguages), Array.prototype);
    }
    if (hasStorage) {
      strictEqual(navigator.storage instanceof StorageManager, true);
    }
    // Either way it is a Navigator to sendBeacon().
    strictEqual(sendBeacon.call(navigator, 'http://sink/first-read'), true);

    // The attributes are configurable, so user code can redefine and
    // delete them.
    const redefine = (name, value) => {
      Object.defineProperty(navigator, name, { value });
      strictEqual(navigator[name], value);
      strictEqual(attributesOf(navigator, name), 'dataec');
    };
    redefine('hardwareConcurrency', 8);
    redefine('userAgent', 'changed');
    deepStrictEqual(ownKeys(navigator), kExpectedKeys);
    if (hasLanguage) {
      const replacement = ['fr'];
      redefine('languages', replacement);
      // Now an ordinary data property: the same array on each read.
      strictEqual(navigator.languages, replacement);
      strictEqual(navigator.languages, navigator.languages);
    }
    if (hasStorage) {
      redefine('storage', 'gone');
    }

    strictEqual(delete navigator.platform, true);
    strictEqual('platform' in navigator, false);
    strictEqual(navigator.platform, undefined);
    deepStrictEqual(
      ownKeys(navigator),
      kExpectedKeys.filter((key) => key !== 'platform')
    );
    if (hasLanguage) {
      strictEqual(delete navigator.languages, true);
      strictEqual(navigator.languages, undefined);
    }

    // New properties can be added, last, and removed.
    navigator.extra = 1;
    strictEqual(attributesOf(navigator, 'extra'), 'datawec');
    strictEqual(ownKeys(navigator).at(-1), 'extra');
    strictEqual(delete navigator.extra, true);

    // Made writable, an attribute can be assigned.
    Object.defineProperty(navigator, 'userAgent', { writable: true });
    navigator.userAgent = 'assigned';
    strictEqual(navigator.userAgent, 'assigned');

    // Frozen, the navigator is still one to sendBeacon().
    Object.freeze(navigator);
    strictEqual(Object.isFrozen(navigator), true);
    strictEqual(sendBeacon.call(navigator, 'http://sink/frozen'), true);
  },
};
