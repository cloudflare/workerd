// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A worker of its own, whose top level replaces built-ins before the first
// read of `scheduler`, which is what loads the TypeScript implementation;
// and, as nothing else runs in this worker, the changes to the global that
// cannot be undone.
//
// While built-ins are replaced, this module uses only what it captured
// first, syntax, and index loops. node:assert is imported by the test, not
// here.

import { env } from 'cloudflare:workers';

const {
  defineProperty,
  getOwnPropertyDescriptor,
  getPrototypeOf,
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
  [Object, 'hasOwn'],
  [Error, 'captureStackTrace'],
  [Reflect, 'defineProperty'],
  [Promise, 'reject'],
  [Function.prototype, 'call'],
  [Function.prototype, 'apply'],
  [Function.prototype, 'bind'],
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
  'wait',
  'scheduler',
  'signal',
  'set_tostring_tag',
  'immutable_api_prototypes',
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
let firstProto;
let firstProtoKeys;
try {
  firstRead = globalThis.scheduler;
  firstKeys = ownKeys(firstRead);
  firstProto = getPrototypeOf(firstRead);
  firstProtoKeys = ownKeys(firstProto);
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
      strictEqual(definePropertyCalls.length, 1);
      const { target, name, descriptor } = definePropertyCalls[0];
      strictEqual(name, '__esModule');
      deepStrictEqual(ownKeys(target), []);
      strictEqual(getPrototypeOf(target), ObjectPrototype);
      deepStrictEqual(ownKeys(descriptor), ['value']);
      strictEqual(descriptor.value, true);
    } else {
      strictEqual(definePropertyCalls.length, 0);
    }

    // The first read, among replaced built-ins.
    strictEqual(firstReadError, undefined);
    strictEqual(accessorRan, false);
    strictEqual(firstRead, scheduler);
    deepStrictEqual(firstKeys, []);
    strictEqual(attributesOf(globalThis, 'scheduler'), 'datawec');

    // The object's shape follows the flags that are set, not what
    // Object.prototype answered for them.
    const flags = Cloudflare.compatibilityFlags;
    const Constructor = firstProto.constructor;
    strictEqual(getPrototypeOf(firstProto), ObjectPrototype);
    strictEqual(firstProtoKeys[0], 'wait');
    strictEqual(firstProtoKeys[1], 'constructor');
    strictEqual(
      Object.hasOwn(firstProto, Symbol.toStringTag),
      flags['set_tostring_tag']
    );
    strictEqual(
      attributesOf(Constructor, 'prototype'),
      flags['immutable_api_prototypes'] ? 'data' : 'dataw'
    );
    strictEqual(await scheduler.wait(1), undefined);

    // wait() keeps working on the object once the global names something
    // else, and deleted, the global is gone: nothing makes another.
    const original = scheduler;
    globalThis.scheduler = undefined;
    strictEqual(await original.wait(1), undefined);
    strictEqual(delete globalThis.scheduler, true);
    strictEqual('scheduler' in globalThis, false);
    strictEqual(typeof scheduler, 'undefined');
    strictEqual(await original.wait(1), undefined);
  },
};
