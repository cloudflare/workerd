// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where the `Cloudflare` global is, and its shape: the object, its
// prototype, and the constructor that only the object leads to.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { inspect } from 'node:util';
import { hasToStringTag, immutablePrototypes, usingTsImpl } from 'which-impl';
import {
  attributesOf,
  caught,
  framesOf,
  isBootstrapFrame,
  throwsTypeError,
} from 'helpers';

const Constructor = Cloudflare.constructor;
const kPrototype = Object.getPrototypeOf(Cloudflare);

export const globalPlacement = {
  test() {
    // An own, writable, enumerable, configurable data property of the
    // global.
    strictEqual(attributesOf(globalThis, 'Cloudflare'), 'datawec');
    // Ledger: the C++ one is where global-scope.h registers it, next to
    // `performance`; the TypeScript one comes after the properties JSG
    // registers on the global.
    const ownKeys = Reflect.ownKeys(globalThis);
    const distance =
      ownKeys.indexOf('Cloudflare') - ownKeys.indexOf('performance');
    if (usingTsImpl) {
      ok(distance > 1);
      ok(ownKeys.indexOf('Cloudflare') > ownKeys.indexOf('console'));
    } else {
      strictEqual(Math.abs(distance), 1);
    }
    const scope = Object.getPrototypeOf(globalThis);
    strictEqual(attributesOf(scope, 'Cloudflare'), 'none');
    strictEqual('Cloudflare' in scope, false);
    const enumerated = [];
    for (const key in globalThis) enumerated.push(key);
    strictEqual(enumerated.filter((key) => key === 'Cloudflare').length, 1);

    // The global names the object, not a class.
    strictEqual(typeof Cloudflare, 'object');
  },
};

export const sameObjectEveryTime = {
  test() {
    strictEqual(Cloudflare, Cloudflare);
    strictEqual(globalThis.Cloudflare, self.Cloudflare);
    // Read through an object that inherits from the global.
    const derived = Object.create(globalThis);
    strictEqual(derived.Cloudflare, Cloudflare);
    strictEqual(Object.hasOwn(derived, 'Cloudflare'), false);
  },
};

export const replaceable = {
  test() {
    const original = Cloudflare;
    try {
      globalThis.Cloudflare = 5;
      strictEqual(Cloudflare, 5);
      strictEqual(attributesOf(globalThis, 'Cloudflare'), 'datawec');
    } finally {
      globalThis.Cloudflare = original;
    }
    strictEqual(Cloudflare, original);
    // A module can declare its own.
    {
      const Cloudflare = 1;
      strictEqual(Cloudflare, 1);
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
    strictEqual(Constructor.name, 'Cloudflare');
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
    throwsTypeError(() => Constructor.call(Cloudflare), 'Illegal constructor');
    throwsTypeError(
      () => Reflect.construct(Constructor, [], Object),
      'Illegal constructor'
    );
    class Subclass extends Constructor {}
    throwsTypeError(() => new Subclass(), 'Illegal constructor');
    // An object made from the prototype has no flags.
    const fake = Object.create(kPrototype);
    strictEqual(fake instanceof Constructor, true);
    deepStrictEqual(Reflect.ownKeys(fake), []);
    strictEqual(fake.compatibilityFlags, undefined);
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
    // A subclass's constructor is the first frame.
    class Subclass extends Constructor {}
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

export const illegalConstructorAmongReplacedBuiltIns = {
  test() {
    // The constructor depends on nothing user code can replace. While
    // built-ins are replaced, only syntax and what was captured before.
    const { defineProperty, getOwnPropertyDescriptor } = Object;
    const kCalled = new Error('a replaced built-in was called');
    class Fake {
      constructor() {
        throw kCalled;
      }
    }
    const replacements = [
      [globalThis, 'TypeError', Fake],
      [globalThis, 'Error', Fake],
      [
        Error,
        'captureStackTrace',
        () => {
          throw kCalled;
        },
      ],
    ];
    const saved = [];
    for (let i = 0; i < replacements.length; i++) {
      saved[i] = getOwnPropertyDescriptor(
        replacements[i][0],
        replacements[i][1]
      );
    }
    let error;
    try {
      for (let i = 0; i < replacements.length; i++) {
        defineProperty(replacements[i][0], replacements[i][1], {
          __proto__: null,
          value: replacements[i][2],
          writable: true,
          configurable: true,
        });
      }
      try {
        new Constructor();
      } catch (thrown) {
        error = thrown;
      }
    } finally {
      for (let i = replacements.length - 1; i >= 0; i--) {
        defineProperty(replacements[i][0], replacements[i][1], saved[i]);
      }
    }
    ok(error !== kCalled);
    ok(error instanceof TypeError);
    strictEqual(error.message, 'Illegal constructor');
    ok(!framesOf(error).some(isBootstrapFrame), error.stack);
  },
};

export const prototypeShape = {
  test() {
    strictEqual(Object.getPrototypeOf(kPrototype), Object.prototype);
    strictEqual(kPrototype.constructor, Constructor);
    const keys = Reflect.ownKeys(kPrototype);
    // JSG's order: the constructor, then its marks.
    strictEqual(keys.length, hasToStringTag ? 4 : 3);
    strictEqual(keys[0], 'constructor');
    strictEqual(attributesOf(kPrototype, 'constructor'), 'datawc');
    let next = 1;
    if (hasToStringTag) {
      strictEqual(keys[next++], Symbol.toStringTag);
      strictEqual(kPrototype[Symbol.toStringTag], 'Cloudflare');
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
    // The flags are no accessor of the prototype.
    strictEqual(Object.hasOwn(kPrototype, 'compatibilityFlags'), false);
  },
};

export const instanceShape = {
  test() {
    strictEqual(Object.getPrototypeOf(Cloudflare), Constructor.prototype);
    strictEqual(Cloudflare instanceof Constructor, true);
    // Its one attribute is an own read-only data property.
    deepStrictEqual(Reflect.ownKeys(Cloudflare), ['compatibilityFlags']);
    strictEqual(attributesOf(Cloudflare, 'compatibilityFlags'), 'dataec');
    strictEqual(Object.isExtensible(Cloudflare), true);
    strictEqual(Object.isFrozen(Cloudflare), false);
    strictEqual(Object.isSealed(Cloudflare), false);
  },
};

export const toStringTag = {
  test() {
    const expected = hasToStringTag ? '[object Cloudflare]' : '[object Object]';
    strictEqual(String(Cloudflare), expected);
    strictEqual(Object.prototype.toString.call(Cloudflare), expected);
    strictEqual(Object.prototype.toString.call(kPrototype), expected);
  },
};

export const sourceText = {
  test() {
    // Ledger: a native function has no source to show.
    const source = Function.prototype.toString.call(Constructor);
    if (usingTsImpl) {
      ok(source.startsWith('function Cloudflare('));
      ok(!source.includes('[native code]'));
    } else {
      strictEqual(source, 'function Cloudflare() { [native code] }');
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
        "'function Cloudflare() {";
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
      strictEqual(Object.getPrototypeOf(Cloudflare), kPrototype);
      strictEqual(Cloudflare instanceof Constructor, false);
    } finally {
      Constructor.prototype = kPrototype;
    }
    strictEqual(Cloudflare instanceof Constructor, true);
  },
};

export const inherited = {
  test() {
    // The flags are a data property, so an object that inherits from the
    // Cloudflare object reads them, whatever it is.
    const derived = Object.create(Cloudflare);
    deepStrictEqual(Reflect.ownKeys(derived), []);
    strictEqual(derived.compatibilityFlags, Cloudflare.compatibilityFlags);
    // And, as for any inherited read-only property, cannot assign them.
    throws(() => {
      'use strict';
      derived.compatibilityFlags = {};
    }, TypeError);
    deepStrictEqual(Reflect.ownKeys(derived), []);
    // Without its prototype the object keeps them.
    try {
      Object.setPrototypeOf(Cloudflare, null);
      strictEqual(typeof Cloudflare.compatibilityFlags, 'object');
      strictEqual(Cloudflare.constructor, undefined);
    } finally {
      Object.setPrototypeOf(Cloudflare, kPrototype);
    }
  },
};

export const readOnly = {
  test() {
    const flags = Cloudflare.compatibilityFlags;
    throwsTypeError(() => {
      'use strict';
      Cloudflare.compatibilityFlags = {};
    }, "Cannot assign to read only property 'compatibilityFlags' of object '#<Cloudflare>'");
    strictEqual(Reflect.set(Cloudflare, 'compatibilityFlags', {}), false);
    strictEqual(Cloudflare.compatibilityFlags, flags);
    // The property is configurable, so user code can redefine it, and the
    // object is extensible, so it can add to it.
    const replacement = {};
    try {
      Object.defineProperty(Cloudflare, 'compatibilityFlags', {
        value: replacement,
      });
      strictEqual(Cloudflare.compatibilityFlags, replacement);
      strictEqual(attributesOf(Cloudflare, 'compatibilityFlags'), 'dataec');
      Cloudflare.extra = 1;
      strictEqual(attributesOf(Cloudflare, 'extra'), 'datawec');
      deepStrictEqual(Reflect.ownKeys(Cloudflare), [
        'compatibilityFlags',
        'extra',
      ]);
    } finally {
      delete Cloudflare.extra;
      Object.defineProperty(Cloudflare, 'compatibilityFlags', { value: flags });
    }
    strictEqual(Cloudflare.compatibilityFlags, flags);
    deepStrictEqual(Reflect.ownKeys(Cloudflare), ['compatibilityFlags']);
  },
};

export const inspected = {
  test() {
    strictEqual(
      inspect(Cloudflare, { depth: 0 }),
      'Cloudflare { compatibilityFlags: [Object: null prototype] }'
    );
    strictEqual(inspect(Constructor), '[Function: Cloudflare]');
    strictEqual(
      inspect(kPrototype),
      hasToStringTag ? 'Object [Cloudflare] {}' : '{}'
    );
  },
};

export const json = {
  test() {
    deepStrictEqual(JSON.parse(JSON.stringify(Cloudflare)), {
      compatibilityFlags: { ...Cloudflare.compatibilityFlags },
    });
  },
};

export const notSerializable = {
  test() {
    throws(
      () => structuredClone(Cloudflare),
      (error) => {
        ok(error instanceof DOMException);
        strictEqual(error.name, 'DataCloneError');
        strictEqual(
          error.message,
          'Could not serialize object of type "Cloudflare". This type does ' +
            'not support serialization.'
        );
        return true;
      }
    );
    // An object that inherits from it is an ordinary object.
    const clone = structuredClone(Object.create(Cloudflare));
    deepStrictEqual(Reflect.ownKeys(clone), []);
    strictEqual(Object.getPrototypeOf(clone), Object.prototype);
  },
};
