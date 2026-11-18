// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Where atob() and btoa() are, and what kind of functions they are.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { attributesOf } from 'helpers';
import { specCompliantPropertyAttributes, usingTsImpl } from 'which-impl';

const kNames = ['atob', 'btoa'];

export const globalPlacement = {
  test() {
    // Writable, enumerable, configurable data properties of the global
    // scope's prototype, whatever the flags, and not own properties of the
    // global.
    const proto = Object.getPrototypeOf(globalThis);
    strictEqual(proto.constructor.name, 'ServiceWorkerGlobalScope');
    for (const name of kNames) {
      strictEqual(attributesOf(globalThis, name), 'none');
      strictEqual(attributesOf(proto, name), 'datawec');
      strictEqual(proto[name], globalThis[name]);
      strictEqual(self[name], globalThis[name]);
      ok(name in globalThis);
      ok(!Object.keys(globalThis).includes(name));
      let enumerated = false;
      for (const key in globalThis) enumerated ||= key === name;
      ok(enumerated);
    }
    // btoa comes first.
    const keys = Object.getOwnPropertyNames(proto);
    strictEqual(keys.indexOf('atob'), keys.indexOf('btoa') + 1);
    // Ledger #3: where among the prototype's properties, which is the
    // order `for...in` over the global visits them in.
    if (usingTsImpl) {
      ok(keys.indexOf('btoa') > keys.indexOf('structuredClone'));
    } else {
      strictEqual(keys[keys.indexOf('atob') + 1], 'setTimeout');
      ok(keys.indexOf('btoa') < keys.indexOf('structuredClone'));
    }
  },
};

export const functionShape = {
  test() {
    for (const name of kNames) {
      const fn = globalThis[name];
      strictEqual(typeof fn, 'function');
      strictEqual(Object.getPrototypeOf(fn), Function.prototype);
      deepStrictEqual(Reflect.ownKeys(fn), ['length', 'name']);
      strictEqual(fn.name, name);
      strictEqual(attributesOf(fn, 'name'), 'datac');
      // The required argument counts only under
      // spec_compliant_property_attributes.
      strictEqual(fn.length, specCompliantPropertyAttributes ? 1 : 0);
      strictEqual(attributesOf(fn, 'length'), 'datac');
      ok(!('prototype' in fn));
      ok(!('caller' in Object.getOwnPropertyNames(fn)));
      ok(Object.isExtensible(fn));
    }
    ok(atob !== btoa);
  },
};

export const notConstructors = {
  test() {
    throws(() => new atob('YQ=='), {
      name: 'TypeError',
      message: 'atob is not a constructor',
    });
    throws(() => new btoa('a'), {
      name: 'TypeError',
      message: 'btoa is not a constructor',
    });
    for (const name of kNames) {
      throws(() => Reflect.construct(globalThis[name], ['']), TypeError);
      throws(() => Reflect.construct(Object, [], globalThis[name]), TypeError);
      throws(() => class extends globalThis[name] {}, TypeError);
    }
  },
};

export const sourceText = {
  test() {
    // Ledger #1.
    for (const name of kNames) {
      const source = Function.prototype.toString.call(globalThis[name]);
      if (usingTsImpl) {
        ok(source.startsWith(`${name}(`), source);
        ok(!source.includes('[native code]'));
      } else {
        strictEqual(source, `function ${name}() { [native code] }`);
      }
    }
  },
};

export const replaceable = {
  test() {
    // Assignment shadows the prototype's property with an own one; deleting
    // that brings the original back.
    for (const name of kNames) {
      const original = globalThis[name];
      const replacement = () => 'replaced';
      globalThis[name] = replacement;
      try {
        strictEqual(attributesOf(globalThis, name), 'datawec');
        strictEqual(globalThis[name], replacement);
        strictEqual(Object.getPrototypeOf(globalThis)[name], original);
      } finally {
        ok(Reflect.deleteProperty(globalThis, name));
      }
      strictEqual(globalThis[name], original);
    }
  },
};

export const sameFunctionEveryTime = {
  test() {
    for (const name of kNames) {
      strictEqual(globalThis[name], globalThis[name]);
      const fn = globalThis[name];
      fn.extra = 1;
      try {
        strictEqual(globalThis[name].extra, 1);
      } finally {
        delete fn.extra;
      }
    }
  },
};
