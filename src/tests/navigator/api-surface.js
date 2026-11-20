// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where the `navigator` global and the Navigator constructor are, and their
// shape: constructor, prototype and instance.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import { inspect } from 'node:util';
import {
  hasLanguage,
  hasStorage,
  hasToStringTag,
  immutablePrototypes,
  specCompliantPropertyAttributes,
  usingTsImpl,
} from 'which-impl';
import {
  attributesOf,
  caught,
  framesOf,
  isBootstrapFrame,
  throwsTypeError,
} from 'helpers';

// The instance's own keys before any test changes them.
const kInstanceKeys = Reflect.ownKeys(navigator);
const kInstanceAttributes = kInstanceKeys.map((key) =>
  attributesOf(navigator, key)
);

const kExpectedInstanceKeys = [
  ...(hasStorage ? ['storage'] : []),
  ...(hasLanguage ? ['languages', 'language'] : []),
  'platform',
  'hardwareConcurrency',
  'userAgent',
];

export const globalPlacement = {
  test() {
    // The instance: an own, writable, enumerable, configurable data
    // property of the global, where global-scope.h registers it.
    strictEqual(attributesOf(globalThis, 'navigator'), 'datawec');
    const ownKeys = Reflect.ownKeys(globalThis);
    strictEqual(ownKeys.indexOf('navigator'), ownKeys.indexOf('console') + 1);
    const scope = Object.getPrototypeOf(globalThis);
    strictEqual(attributesOf(scope, 'navigator'), 'none');

    // The constructor: on the global scope's prototype, and under
    // spec_compliant_property_attributes also the global's own.
    strictEqual(attributesOf(scope, 'Navigator'), 'datawec');
    strictEqual(
      attributesOf(globalThis, 'Navigator'),
      specCompliantPropertyAttributes ? 'datawc' : 'none'
    );
    strictEqual(scope.Navigator, Navigator);

    // Ledger: the TypeScript constructor comes after the properties JSG
    // registers on the global scope's prototype.
    const scopeKeys = Reflect.ownKeys(scope);
    const index = scopeKeys.indexOf('Navigator');
    if (usingTsImpl) {
      ok(index > scopeKeys.indexOf('URL'));
      ok(index > scopeKeys.indexOf('TextEncoder') + 1);
    } else {
      strictEqual(index, scopeKeys.indexOf('TextEncoder') + 1);
    }
  },
};

export const sameObjectEveryTime = {
  test() {
    strictEqual(navigator, navigator);
    strictEqual(globalThis.navigator, self.navigator);
    // Read through an object that inherits from the global.
    const derived = Object.create(globalThis);
    strictEqual(derived.navigator, navigator);
    strictEqual(Object.hasOwn(derived, 'navigator'), false);
  },
};

export const replaceable = {
  test() {
    const original = navigator;
    try {
      globalThis.navigator = 5;
      strictEqual(navigator, 5);
      strictEqual(attributesOf(globalThis, 'navigator'), 'datawec');
    } finally {
      globalThis.navigator = original;
    }
    strictEqual(navigator, original);
  },
};

export const constructorShape = {
  test() {
    strictEqual(typeof Navigator, 'function');
    strictEqual(Object.getPrototypeOf(Navigator), Function.prototype);
    deepStrictEqual(Reflect.ownKeys(Navigator), [
      'length',
      'name',
      'prototype',
    ]);
    strictEqual(Navigator.name, 'Navigator');
    strictEqual(Navigator.length, 0);
    strictEqual(attributesOf(Navigator, 'name'), 'datac');
    strictEqual(attributesOf(Navigator, 'length'), 'datac');
    strictEqual(
      attributesOf(Navigator, 'prototype'),
      immutablePrototypes ? 'data' : 'dataw'
    );
  },
};

export const illegalConstructor = {
  test() {
    // Constructed, called, or reached by a subclass: the same error.
    throwsTypeError(() => new Navigator(), 'Illegal constructor');
    throwsTypeError(() => Navigator(), 'Illegal constructor');
    throwsTypeError(() => Navigator.call(navigator), 'Illegal constructor');
    throwsTypeError(
      () => Reflect.construct(Navigator, [], Object),
      'Illegal constructor'
    );
    class Subclass extends Navigator {}
    throwsTypeError(() => new Subclass(), 'Illegal constructor');
    // No instance can be made from the prototype either: it has none of
    // the attributes, and is not a receiver sendBeacon() accepts.
    const fake = Object.create(Navigator.prototype);
    strictEqual(fake instanceof Navigator, true);
    deepStrictEqual(Reflect.ownKeys(fake), []);
    strictEqual(fake.userAgent, undefined);
  },
};

export const illegalConstructorStack = {
  test() {
    // The error's stack starts at the caller, in both implementations.
    function constructs() {
      return caught(() => new Navigator());
    }
    const error = constructs();
    const frames = framesOf(error);
    ok(!frames.some(isBootstrapFrame), error.stack);
    // The function constructing, caught(), then constructs().
    ok(frames[1].includes('caught'), error.stack);
    ok(frames[2].includes('constructs'), error.stack);
    // A subclass's constructor is the first frame.
    class Subclass extends Navigator {}
    const subclassError = caught(() => new Subclass());
    ok(
      framesOf(subclassError)[0].includes('new Subclass'),
      subclassError.stack
    );
    ok(!framesOf(subclassError).some(isBootstrapFrame), subclassError.stack);
    // A fresh error each time.
    ok(error !== constructs());
  },
};

export const prototypeShape = {
  test() {
    const proto = Navigator.prototype;
    strictEqual(Object.getPrototypeOf(proto), Object.prototype);
    strictEqual(proto.constructor, Navigator);
    const keys = Reflect.ownKeys(proto);
    // JSG's order: the method, the constructor, then its marks.
    strictEqual(keys.length, hasToStringTag ? 5 : 4);
    strictEqual(keys[0], 'sendBeacon');
    strictEqual(keys[1], 'constructor');
    strictEqual(attributesOf(proto, 'sendBeacon'), 'datawec');
    strictEqual(attributesOf(proto, 'constructor'), 'datawc');
    let next = 2;
    if (hasToStringTag) {
      strictEqual(keys[next++], Symbol.toStringTag);
      strictEqual(proto[Symbol.toStringTag], 'Navigator');
      strictEqual(attributesOf(proto, Symbol.toStringTag), 'datawc');
    } else {
      strictEqual(Object.hasOwn(proto, Symbol.toStringTag), false);
    }
    const internalClass = Symbol.for('cloudflare:internal-class');
    strictEqual(keys[next++], internalClass);
    strictEqual(proto[internalClass], internalClass);
    strictEqual(attributesOf(proto, internalClass), 'data');
    const inspectKey = keys[next];
    strictEqual(typeof inspectKey, 'symbol');
    strictEqual(inspectKey.description, 'kResourceTypeInspect');
    strictEqual(attributesOf(proto, inspectKey), 'datac');
    // No attribute is an accessor of the prototype.
    for (const key of kExpectedInstanceKeys) {
      strictEqual(Object.hasOwn(proto, key), false, key);
    }
  },
};

export const instanceShape = {
  test() {
    strictEqual(typeof navigator, 'object');
    strictEqual(Object.getPrototypeOf(navigator), Navigator.prototype);
    strictEqual(navigator instanceof Navigator, true);
    // Every attribute is an own read-only data property, in the reverse of
    // the order in which global-scope.h registers them.
    deepStrictEqual(kInstanceKeys, kExpectedInstanceKeys);
    deepStrictEqual(
      kInstanceAttributes,
      kExpectedInstanceKeys.map(() => 'dataec')
    );
    strictEqual(Object.isExtensible(navigator), true);
    strictEqual(Object.isFrozen(navigator), false);
    strictEqual(Object.isSealed(navigator), false);
  },
};

export const toStringTag = {
  test() {
    const expected = hasToStringTag ? '[object Navigator]' : '[object Object]';
    strictEqual(String(navigator), expected);
    strictEqual(Object.prototype.toString.call(navigator), expected);
    strictEqual(Object.prototype.toString.call(Navigator.prototype), expected);
  },
};

export const sourceText = {
  test() {
    // Ledger: a native function has no source to show.
    const constructorSource = Function.prototype.toString.call(Navigator);
    const methodSource = Function.prototype.toString.call(
      Navigator.prototype.sendBeacon
    );
    if (usingTsImpl) {
      ok(constructorSource.startsWith('function Navigator('));
      ok(!constructorSource.includes('[native code]'));
      ok(methodSource.startsWith('sendBeacon('));
      ok(!methodSource.includes('[native code]'));
    } else {
      strictEqual(constructorSource, 'function Navigator() { [native code] }');
      strictEqual(methodSource, 'function sendBeacon() { [native code] }');
    }
  },
};

export const prototypeProperty = {
  test() {
    const proto = Navigator.prototype;
    const { sendBeacon } = proto;
    const replacement = {};
    if (immutablePrototypes) {
      throwsTypeError(
        () => {
          'use strict';
          Navigator.prototype = replacement;
        },
        "Cannot assign to read only property 'prototype' of function '" +
          Function.prototype.toString.call(Navigator) +
          "'"
      );
      strictEqual(Navigator.prototype, proto);
      return;
    }
    try {
      Navigator.prototype = replacement;
      strictEqual(Navigator.prototype, replacement);
      // The instance keeps the prototype it was made with, and stays a
      // Navigator to sendBeacon().
      strictEqual(Object.getPrototypeOf(navigator), proto);
      strictEqual(navigator instanceof Navigator, false);
      strictEqual(
        sendBeacon.call(navigator, 'http://sink/prototype-property'),
        true
      );
    } finally {
      Navigator.prototype = proto;
    }
    strictEqual(navigator instanceof Navigator, true);
  },
};

export const prototypeSwap = {
  test() {
    const proto = Navigator.prototype;
    const { sendBeacon } = proto;
    try {
      // Without its prototype the instance keeps its attributes, which are
      // its own, and is still a Navigator to sendBeacon().
      Object.setPrototypeOf(navigator, null);
      deepStrictEqual(Reflect.ownKeys(navigator), kInstanceKeys);
      strictEqual(navigator.userAgent, 'Cloudflare-Workers');
      strictEqual(navigator.sendBeacon, undefined);
      strictEqual(
        sendBeacon.call(navigator, 'http://sink/prototype-swap'),
        true
      );
    } finally {
      Object.setPrototypeOf(navigator, proto);
    }
    // An object given the prototype is no Navigator.
    const impostor = Object.setPrototypeOf({ userAgent: 'x' }, proto);
    strictEqual(impostor instanceof Navigator, true);
    ok(
      caught(() => impostor.sendBeacon('http://sink/impostor')) instanceof
        TypeError
    );
  },
};

export const inspected = {
  test() {
    const lines = ['Navigator {'];
    if (hasStorage) lines.push('  storage: StorageManager {},');
    if (hasLanguage) {
      lines.push("  languages: [ 'en', [length]: 1 ],");
      lines.push("  language: 'en',");
    }
    lines.push("  platform: '',");
    lines.push('  hardwareConcurrency: 1,');
    lines.push("  userAgent: 'Cloudflare-Workers'");
    lines.push('}');
    strictEqual(inspect(navigator), lines.join('\n'));
    strictEqual(inspect(Navigator), '[Function: Navigator]');
    strictEqual(
      inspect(Navigator.prototype),
      hasToStringTag
        ? 'Object [Navigator] { sendBeacon: [Function: sendBeacon] }'
        : '{ sendBeacon: [Function: sendBeacon] }'
    );
  },
};
