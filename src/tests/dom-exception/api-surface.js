// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shape of the constructor, the prototype and instances, and where the
// global lives.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import {
  usingTsImpl,
  setToStringTag,
  specCompliantPropertyAttributes,
  webIdlAttributes,
  nonWritablePrototype,
} from 'which-impl';
import { attributesOf, kLegacyCodes, instanceKeys } from 'helpers';

const kConstantNames = kLegacyCodes.map(([constant]) => constant);

export const constructorShape = {
  test() {
    strictEqual(typeof DOMException, 'function');
    strictEqual(DOMException.name, 'DOMException');
    strictEqual(DOMException.length, 0);
    strictEqual(Object.getPrototypeOf(DOMException), Function.prototype);
    strictEqual(attributesOf(DOMException, 'length'), 'datac');
    strictEqual(attributesOf(DOMException, 'name'), 'datac');
    // Ledger #10: JSG leaves the constructor's `prototype` writable unless
    // immutable_api_prototypes is set.
    strictEqual(
      attributesOf(DOMException, 'prototype'),
      nonWritablePrototype ? 'data' : 'dataw'
    );
    deepStrictEqual(
      Reflect.ownKeys(DOMException).map(String).sort(),
      ['length', 'name', 'prototype', ...kConstantNames].sort()
    );
  },
};

export const prototypeShape = {
  test() {
    const proto = DOMException.prototype;
    strictEqual(Object.getPrototypeOf(proto), Error.prototype);
    strictEqual(proto.constructor, DOMException);
    strictEqual(attributesOf(proto, 'constructor'), 'datawc');

    // Ledger #1: WebIDL attributes are prototype accessors; the C++ makes
    // them own properties of each instance. Ledger #5: JSG's inspect hook.
    const symbols = Object.getOwnPropertySymbols(proto).map(String);
    const expectedSymbols = ['Symbol(cloudflare:internal-class)'];
    if (setToStringTag) expectedSymbols.push('Symbol(Symbol.toStringTag)');
    if (!usingTsImpl) expectedSymbols.push('Symbol(kResourceTypeInspect)');
    deepStrictEqual(symbols.sort(), expectedSymbols.sort());
    const attributeNames = usingTsImpl ? ['code', 'message', 'name'] : [];
    deepStrictEqual(
      Object.getOwnPropertyNames(proto).sort(),
      ['constructor', ...attributeNames, ...kConstantNames].sort()
    );
  },
};

export const attributeAccessors = {
  test() {
    for (const attribute of ['name', 'message', 'code']) {
      const descriptor = Object.getOwnPropertyDescriptor(
        DOMException.prototype,
        attribute
      );
      if (!usingTsImpl) {
        strictEqual(descriptor, undefined);
        continue;
      }
      strictEqual(
        attributesOf(DOMException.prototype, attribute),
        'accessorec'
      );
      strictEqual(descriptor.set, undefined);
      strictEqual(descriptor.get.name, `get ${attribute}`);
      strictEqual(descriptor.get.length, 0);
    }
  },
};

export const legacyCodeConstants = {
  test() {
    for (const [constant, value] of kLegacyCodes) {
      strictEqual(DOMException[constant], value);
      strictEqual(DOMException.prototype[constant], value);
      // Ledger #7: WebIDL constants are not configurable.
      const expected = webIdlAttributes ? 'datae' : 'dataec';
      strictEqual(attributesOf(DOMException, constant), expected);
      strictEqual(attributesOf(DOMException.prototype, constant), expected);
    }
  },
};

export const toStringTag = {
  test() {
    if (!setToStringTag) {
      strictEqual(
        attributesOf(DOMException.prototype, Symbol.toStringTag),
        'none'
      );
      return;
    }
    strictEqual(DOMException.prototype[Symbol.toStringTag], 'DOMException');
    // Ledger #11: JSG's tag is writable.
    strictEqual(
      attributesOf(DOMException.prototype, Symbol.toStringTag),
      usingTsImpl ? 'datac' : 'datawc'
    );
  },
};

export const instanceShape = {
  test() {
    const error = new DOMException('m', 'AbortError');
    deepStrictEqual(Reflect.ownKeys(error).map(String), instanceKeys());
    if (usingTsImpl) {
      // Ledger #2: V8's own stack.
      strictEqual(attributesOf(error, 'stack'), 'accessorc');
    } else {
      for (const key of ['code', 'name', 'message']) {
        strictEqual(attributesOf(error, key), 'dataec');
      }
      strictEqual(attributesOf(error, 'stack'), 'accessorec');
    }
    ok(Object.isExtensible(error));
  },
};

export const subclassing = {
  test() {
    class Sub extends DOMException {
      constructor() {
        super('m', 'NotFoundError');
      }
    }
    const error = new Sub();
    ok(error instanceof Sub);
    ok(error instanceof DOMException);
    ok(error instanceof Error);
    strictEqual(Object.getPrototypeOf(error), Sub.prototype);
    strictEqual(error.name, 'NotFoundError');
    strictEqual(error.message, 'm');
    strictEqual(error.code, 8);
    deepStrictEqual(Reflect.ownKeys(error).map(String), instanceKeys());
  },
};

export const globalPlacement = {
  test() {
    // Where JSG_NESTED_TYPE puts it, which the TypeScript implementation
    // matches: on the global scope's prototype, and under
    // spec_compliant_property_attributes also an own, non-enumerable
    // property of the global.
    strictEqual(
      attributesOf(Object.getPrototypeOf(globalThis), 'DOMException'),
      'datawec'
    );
    strictEqual(
      attributesOf(globalThis, 'DOMException'),
      specCompliantPropertyAttributes ? 'datawc' : 'none'
    );
  },
};
