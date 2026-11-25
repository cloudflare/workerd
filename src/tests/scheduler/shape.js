// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where the `scheduler` global is, and its shape: the object, its prototype,
// and the constructor that only the object leads to.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';
import {
  hasToStringTag,
  immutablePrototypes,
  specCompliantAttributes,
  usingTsImpl,
} from 'which-impl';
import {
  attributesOf,
  caught,
  framesOf,
  isBootstrapFrame,
  throwsTypeError,
} from 'helpers';

const Constructor = scheduler.constructor;
const kPrototype = Object.getPrototypeOf(scheduler);
const wait = kPrototype.wait;

export const globalPlacement = {
  test() {
    // An own, writable, enumerable, configurable data property of the
    // global.
    strictEqual(attributesOf(globalThis, 'scheduler'), 'datawec');
    // Ledger: the C++ one is where global-scope.h registers it, next to
    // `performance`; the TypeScript one comes after the properties JSG
    // registers on the global.
    const ownKeys = Reflect.ownKeys(globalThis);
    const distance =
      ownKeys.indexOf('scheduler') - ownKeys.indexOf('performance');
    if (usingTsImpl) {
      ok(distance > 1);
      ok(ownKeys.indexOf('scheduler') > ownKeys.indexOf('console'));
    } else {
      strictEqual(Math.abs(distance), 1);
    }
    const scope = Object.getPrototypeOf(globalThis);
    strictEqual(attributesOf(scope, 'scheduler'), 'none');
    strictEqual('scheduler' in scope, false);
    const enumerated = [];
    for (const key in globalThis) enumerated.push(key);
    strictEqual(enumerated.filter((key) => key === 'scheduler').length, 1);

    // The global names the object, and nothing names its class.
    strictEqual(typeof scheduler, 'object');
    strictEqual('Scheduler' in globalThis, false);
  },
};

export const sameObjectEveryTime = {
  test() {
    strictEqual(scheduler, scheduler);
    strictEqual(globalThis.scheduler, self.scheduler);
    // Read through an object that inherits from the global.
    const derived = Object.create(globalThis);
    strictEqual(derived.scheduler, scheduler);
    strictEqual(Object.hasOwn(derived, 'scheduler'), false);
  },
};

export const replaceable = {
  test() {
    const original = scheduler;
    try {
      globalThis.scheduler = 5;
      strictEqual(scheduler, 5);
      strictEqual(attributesOf(globalThis, 'scheduler'), 'datawec');
    } finally {
      globalThis.scheduler = original;
    }
    strictEqual(scheduler, original);
    // A module can declare its own.
    {
      const scheduler = 1;
      strictEqual(scheduler, 1);
    }
  },
};

export const constructorShape = {
  test() {
    strictEqual(typeof Constructor, 'function');
    strictEqual(Object.getPrototypeOf(Constructor), Function.prototype);
    deepStrictEqual(Reflect.ownKeys(Constructor), [
      'length',
      'name',
      'prototype',
    ]);
    strictEqual(Constructor.name, 'Scheduler');
    strictEqual(Constructor.length, 0);
    strictEqual(attributesOf(Constructor, 'name'), 'datac');
    strictEqual(attributesOf(Constructor, 'length'), 'datac');
    strictEqual(
      attributesOf(Constructor, 'prototype'),
      immutablePrototypes ? 'data' : 'dataw'
    );
    strictEqual(Constructor.prototype, kPrototype);
  },
};

export const illegalConstructor = {
  test() {
    // Constructed, called, or reached by a subclass: the same error.
    throwsTypeError(() => new Constructor(), 'Illegal constructor');
    throwsTypeError(() => Constructor(), 'Illegal constructor');
    throwsTypeError(() => Constructor.call(scheduler), 'Illegal constructor');
    throwsTypeError(
      () => Reflect.construct(Constructor, [], Object),
      'Illegal constructor'
    );
    class Subclass extends Constructor {}
    throwsTypeError(() => new Subclass(), 'Illegal constructor');
  },
};

export const illegalConstructorStack = {
  test() {
    // The error's stack starts at the caller, in both implementations.
    function constructs() {
      return caught(() => new Constructor());
    }
    const error = constructs();
    const frames = framesOf(error);
    ok(!frames.some(isBootstrapFrame), error.stack);
    // The function constructing, caught(), then constructs().
    ok(frames[1].includes('caught'), error.stack);
    ok(frames[2].includes('constructs'), error.stack);
    // A fresh error each time.
    ok(error !== constructs());
  },
};

export const prototypeShape = {
  test() {
    strictEqual(Object.getPrototypeOf(kPrototype), Object.prototype);
    strictEqual(kPrototype.constructor, Constructor);
    const keys = Reflect.ownKeys(kPrototype);
    // JSG's order: the method, the constructor, then its marks.
    strictEqual(keys.length, hasToStringTag ? 5 : 4);
    strictEqual(keys[0], 'wait');
    strictEqual(attributesOf(kPrototype, 'wait'), 'datawec');
    strictEqual(keys[1], 'constructor');
    strictEqual(attributesOf(kPrototype, 'constructor'), 'datawc');
    let next = 2;
    if (hasToStringTag) {
      strictEqual(keys[next++], Symbol.toStringTag);
      strictEqual(kPrototype[Symbol.toStringTag], 'Scheduler');
      strictEqual(attributesOf(kPrototype, Symbol.toStringTag), 'datawc');
    } else {
      strictEqual(Object.hasOwn(kPrototype, Symbol.toStringTag), false);
    }
    const internalClass = Symbol.for('cloudflare:internal-class');
    strictEqual(keys[next++], internalClass);
    strictEqual(kPrototype[internalClass], internalClass);
    strictEqual(attributesOf(kPrototype, internalClass), 'data');
    const inspectKey = keys[next];
    strictEqual(typeof inspectKey, 'symbol');
    strictEqual(inspectKey.description, 'kResourceTypeInspect');
    strictEqual(attributesOf(kPrototype, inspectKey), 'datac');
  },
};

export const instanceShape = {
  test() {
    strictEqual(Object.getPrototypeOf(scheduler), Constructor.prototype);
    strictEqual(scheduler instanceof Constructor, true);
    deepStrictEqual(Reflect.ownKeys(scheduler), []);
    strictEqual(Object.isExtensible(scheduler), true);
    strictEqual(scheduler.wait, wait);
  },
};

export const waitShape = {
  test() {
    strictEqual(typeof wait, 'function');
    strictEqual(Object.getPrototypeOf(wait), Function.prototype);
    deepStrictEqual(Reflect.ownKeys(wait), ['length', 'name']);
    strictEqual(wait.name, 'wait');
    strictEqual(wait.length, specCompliantAttributes ? 1 : 0);
    strictEqual(attributesOf(wait, 'name'), 'datac');
    strictEqual(attributesOf(wait, 'length'), 'datac');
    // Not a constructor.
    throws(() => new wait(1), TypeError);
  },
};

export const toStringTag = {
  test() {
    const expected = hasToStringTag ? '[object Scheduler]' : '[object Object]';
    strictEqual(String(scheduler), expected);
    strictEqual(Object.prototype.toString.call(scheduler), expected);
    strictEqual(Object.prototype.toString.call(kPrototype), expected);
  },
};

export const sourceText = {
  test() {
    // Ledger: a native function has no source to show.
    for (const [fn, start, native] of [
      [
        Constructor,
        'function Scheduler(',
        'function Scheduler() { [native code] }',
      ],
      [wait, 'wait(', 'function wait() { [native code] }'],
    ]) {
      const source = Function.prototype.toString.call(fn);
      if (usingTsImpl) {
        ok(source.startsWith(start), source);
        ok(!source.includes('[native code]'));
      } else {
        strictEqual(source, native);
      }
    }
  },
};

export const prototypeProperty = {
  test() {
    const replacement = {};
    if (immutablePrototypes) {
      const error = caught(() => {
        'use strict';
        Constructor.prototype = replacement;
      });
      ok(error instanceof TypeError);
      // Ledger: the message quotes the function's source, which V8
      // abbreviates.
      const message =
        "Cannot assign to read only property 'prototype' of function " +
        "'function Scheduler() {";
      if (usingTsImpl) {
        ok(error.message.startsWith(message), error.message);
        ok(!error.message.includes('[native code]'), error.message);
      } else {
        strictEqual(error.message, `${message} [native code] }'`);
      }
      strictEqual(Constructor.prototype, kPrototype);
      return;
    }
    try {
      Constructor.prototype = replacement;
      strictEqual(Constructor.prototype, replacement);
      // The object keeps the prototype it was made with.
      strictEqual(Object.getPrototypeOf(scheduler), kPrototype);
      strictEqual(scheduler instanceof Constructor, false);
    } finally {
      Constructor.prototype = kPrototype;
    }
    strictEqual(scheduler instanceof Constructor, true);
  },
};

export const inspected = {
  test() {
    strictEqual(inspect(scheduler), 'Scheduler {}');
    strictEqual(inspect(Constructor), '[Function: Scheduler]');
    strictEqual(inspect(wait), '[Function: wait]');
  },
};

export const notSerializable = {
  test() {
    throws(
      () => structuredClone(scheduler),
      (error) => {
        ok(error instanceof DOMException);
        strictEqual(error.name, 'DataCloneError');
        strictEqual(
          error.message,
          'Could not serialize object of type "Scheduler". This type does ' +
            'not support serialization.'
        );
        return true;
      }
    );
  },
};
