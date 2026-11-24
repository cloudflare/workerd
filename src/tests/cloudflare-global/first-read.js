// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A worker of its own, whose top level replaces built-ins before the first
// read of `Cloudflare`, which is what loads the TypeScript implementation
// and makes the object and its flags; and,
// as nothing else runs in this worker, the changes to the object that
// cannot be undone.
//
// While built-ins are replaced, this module uses only what it captured
// first, syntax, and index loops. node:assert is imported by the test, not
// here: the runtime's own Node.js modules read `Cloudflare` as they load.

import { env } from 'cloudflare:workers';
import schema from 'compatibility-date.capnp';

const {
  defineProperty,
  getOwnPropertyDescriptor,
  getPrototypeOf,
  isFrozen,
  prototype: ObjectPrototype,
} = Object;
const { deleteProperty, ownKeys } = Reflect;

const kCalled = new Error('a replaced built-in was called');
function fail() {
  throw kCalled;
}

// The replaced Object.defineProperty: records its calls, and defines
// nothing.
const definePropertyCalls = [];
function recordDefineProperty(target, name, descriptor) {
  // Not an assignment, which would reach the accessor on Object.prototype.
  defineProperty(definePropertyCalls, definePropertyCalls.length, {
    __proto__: null,
    value: { __proto__: null, target, name, descriptor },
    writable: true,
    enumerable: true,
    configurable: true,
  });
  return target;
}

const kStatics = [
  [Object, 'create'],
  [Object, 'defineProperty'],
  [Object, 'defineProperties'],
  [Object, 'setPrototypeOf'],
  [Object, 'freeze'],
  [Object, 'seal'],
  [Object, 'preventExtensions'],
  [Object, 'hasOwn'],
  [Error, 'captureStackTrace'],
  [Reflect, 'defineProperty'],
  [Function.prototype, 'call'],
  [Function.prototype, 'apply'],
  [Array.prototype, 'push'],
  [Array.prototype, Symbol.iterator],
];
// Read or written through Object.prototype by code that takes an object of
// its own, or a descriptor, for one without a prototype. The flag names are
// those the object's shape depends on: an inherited `true` must not stand in
// for a flag that is not set.
const kAccessors = [
  'value',
  'writable',
  'enumerable',
  'configurable',
  'get',
  'set',
  'compatibilityFlags',
  'set_tostring_tag',
  'immutable_api_prototypes',
  'nodejs_compat',
  'not_a_real_compat_flag',
  '0',
  '1',
];

let accessorRan = false;
const savedStatics = [];
for (let i = 0; i < kStatics.length; i++) {
  savedStatics[i] = getOwnPropertyDescriptor(kStatics[i][0], kStatics[i][1]);
  defineProperty(kStatics[i][0], kStatics[i][1], {
    __proto__: null,
    value:
      kStatics[i][0] === Object && kStatics[i][1] === 'defineProperty'
        ? recordDefineProperty
        : fail,
    writable: true,
    configurable: true,
  });
}
for (let i = 0; i < kAccessors.length; i++) {
  defineProperty(ObjectPrototype, kAccessors[i], {
    __proto__: null,
    get() {
      accessorRan = true;
      return true;
    },
    set() {
      accessorRan = true;
    },
    configurable: true,
  });
}

let firstRead;
let firstReadError;
let firstKeys;
let firstFlags;
let firstFlagKeys;
let firstFlagsFrozen;
try {
  firstRead = globalThis.Cloudflare;
  firstKeys = ownKeys(firstRead);
  firstFlags = getOwnPropertyDescriptor(firstRead, 'compatibilityFlags').value;
  firstFlagKeys = ownKeys(firstFlags);
  firstFlagsFrozen = isFrozen(firstFlags);
} catch (error) {
  firstReadError = error;
} finally {
  for (let i = kAccessors.length - 1; i >= 0; i--) {
    deleteProperty(ObjectPrototype, kAccessors[i]);
  }
  for (let i = kStatics.length - 1; i >= 0; i--) {
    defineProperty(kStatics[i][0], kStatics[i][1], savedStatics[i]);
  }
}

const kEnableFlags = Array.from(
  schema.matchAll(/@(\d+)\s*:\s*Bool\b([^;]*);/g),
  (field) => [
    Number(field[1]),
    /\$compatEnableFlag\("([^"]+)"\)/.exec(field[2]),
  ]
)
  .filter(([, name]) => name !== null)
  .sort(([a], [b]) => a - b)
  .map(([, name]) => name[1]);

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
  async test() {
    const { deepStrictEqual, strictEqual } = await import('node:assert');
    // Ledger: the TypeScript implementation loads at the first read, and
    // the code TypeScript compiles a module to begins by calling the global
    // Object.defineProperty on its `exports` object. User code that replaced
    // it is called with that object, which stays empty and which nothing
    // else uses.
    if (env.USING_TS_IMPL === true) {
      // Under the newest compatibility date the runtime has read the global,
      // and so loaded the module, before this worker's top level ran. A flag
      // that only a date sets tells that variant apart.
      const loadedBeforeTopLevel =
        firstFlags['formdata_parser_supports_files'] === true;
      strictEqual(definePropertyCalls.length >= 1, !loadedBeforeTopLevel);
      for (const { target, name, descriptor } of definePropertyCalls) {
        strictEqual(name, '__esModule');
        deepStrictEqual(ownKeys(target), []);
        strictEqual(getPrototypeOf(target), ObjectPrototype);
        deepStrictEqual(ownKeys(descriptor), ['value']);
        strictEqual(descriptor.value, true);
      }
    } else {
      strictEqual(definePropertyCalls.length, 0);
    }

    // The first read, among replaced built-ins.
    strictEqual(firstReadError, undefined);
    strictEqual(accessorRan, false);
    strictEqual(firstRead, Cloudflare);
    deepStrictEqual(firstKeys, ['compatibilityFlags']);
    strictEqual(attributesOf(Cloudflare, 'compatibilityFlags'), 'dataec');
    strictEqual(firstFlags, Cloudflare.compatibilityFlags);
    strictEqual(getPrototypeOf(firstFlags), null);
    strictEqual(firstFlagsFrozen, true);
    deepStrictEqual(firstFlagKeys, kEnableFlags);

    // The object's shape follows the flags that are set, not what
    // Object.prototype answered for them.
    const flags = firstFlags;
    const proto = getPrototypeOf(Cloudflare);
    const Constructor = proto.constructor;
    strictEqual(getPrototypeOf(proto), ObjectPrototype);
    strictEqual(
      Object.hasOwn(proto, Symbol.toStringTag),
      flags['set_tostring_tag']
    );
    strictEqual(
      attributesOf(Constructor, 'prototype'),
      flags['immutable_api_prototypes'] ? 'data' : 'dataw'
    );
    strictEqual(flags['nodejs_compat'], true);
    strictEqual('not_a_real_compat_flag' in flags, false);

    // The attribute is configurable, so user code can delete it, and the
    // object extensible until user code says otherwise.
    Cloudflare.extra = 1;
    strictEqual(ownKeys(Cloudflare).at(-1), 'extra');
    strictEqual(delete Cloudflare.compatibilityFlags, true);
    strictEqual('compatibilityFlags' in Cloudflare, false);
    deepStrictEqual(ownKeys(Cloudflare), ['extra']);
    Object.freeze(Cloudflare);
    strictEqual(Object.isFrozen(Cloudflare), true);
    // The flags it listed are unchanged.
    deepStrictEqual(ownKeys(flags), kEnableFlags);

    // Deleted, the global is gone: nothing makes another.
    strictEqual(delete globalThis.Cloudflare, true);
    strictEqual('Cloudflare' in globalThis, false);
    strictEqual(typeof Cloudflare, 'undefined');
  },
};
