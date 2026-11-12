// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shape of URLPattern: members and their order, attributes, names and
// lengths, Symbol.toStringTag, the runtime class marks and the global's
// placement.

import { deepStrictEqual, strictEqual, ok } from 'node:assert';
import {
  usingTsImpl,
  urlPatternStandard,
  setToStringTag,
  webIdlAttributes,
  nonWritablePrototype,
  specCompliantPropertyAttributes,
} from 'which-impl';
import { attributes, memberNames, kAttributes } from 'helpers';

export const prototypeMembers = {
  test() {
    // WebIDL: the attributes, then the operations. The legacy parser has no
    // hasRegExpGroups.
    deepStrictEqual(memberNames(URLPattern.prototype), [
      ...kAttributes,
      'test',
      'exec',
    ]);
    if (!urlPatternStandard) {
      ok(!('hasRegExpGroups' in URLPattern.prototype));
    }
  },
};

export const memberAttributes = {
  test() {
    const proto = URLPattern.prototype;
    for (const name of ['test', 'exec']) {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(d), 'datawec', name);
      strictEqual(d.value.name, name);
      strictEqual(d.value.length, 0, name);
    }
    for (const name of kAttributes) {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(d), 'accessorec', name);
      strictEqual(d.set, undefined, name);
      // Ledger #8.
      strictEqual(d.get.name, webIdlAttributes ? `get ${name}` : '', name);
      strictEqual(d.get.length, 0, name);
    }
  },
};

export const constructorAttributes = {
  test() {
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(URLPattern, 'name')),
      'datac'
    );
    strictEqual(URLPattern.name, 'URLPattern');
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(URLPattern, 'length')),
      'datac'
    );
    strictEqual(URLPattern.length, 0);
    // Ledger #8.
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(URLPattern, 'prototype')),
      nonWritablePrototype ? 'data' : 'dataw'
    );
    const c = Object.getOwnPropertyDescriptor(
      URLPattern.prototype,
      'constructor'
    );
    strictEqual(attributes(c), 'datawc');
    strictEqual(c.value, URLPattern);
    deepStrictEqual(
      Object.getOwnPropertyNames(URLPattern).sort(),
      ['length', 'name', 'prototype'].sort()
    );
  },
};

export const instancesHaveNoOwnProperties = {
  test() {
    const pattern = new URLPattern({ pathname: '/a' });
    deepStrictEqual(Reflect.ownKeys(pattern), []);
    deepStrictEqual({ ...pattern }, {});
    strictEqual(pattern.constructor.name, 'URLPattern');
  },
};

export const subclassing = {
  test() {
    class Sub extends URLPattern {
      get pathname() {
        return 'override';
      }
    }
    const pattern = new Sub({ pathname: '/a' });
    ok(pattern instanceof URLPattern);
    strictEqual(Object.getPrototypeOf(pattern), Sub.prototype);
    strictEqual(pattern.pathname, 'override');
    // The implementation reads its own state, not the overridden getter.
    strictEqual(pattern.test({ pathname: '/a' }), true);
    strictEqual(pattern.exec({ pathname: '/a' }).pathname.input, '/a');
  },
};

export const toStringTagBranding = {
  test() {
    const tag = (v) => Object.prototype.toString.call(v);
    const pattern = new URLPattern({});
    if (!setToStringTag) {
      strictEqual(tag(pattern), '[object Object]');
      ok(!Object.hasOwn(URLPattern.prototype, Symbol.toStringTag));
      return;
    }
    strictEqual(tag(pattern), '[object URLPattern]');
    const d = Object.getOwnPropertyDescriptor(
      URLPattern.prototype,
      Symbol.toStringTag
    );
    strictEqual(d.value, 'URLPattern');
    // Ledger #8.
    strictEqual(attributes(d), usingTsImpl ? 'datac' : 'datawc');
  },
};

export const runtimeClassMarks = {
  test() {
    const proto = URLPattern.prototype;
    const kInternalClass = Symbol.for('cloudflare:internal-class');
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(proto, kInternalClass)),
      'data'
    );
    const inspect = Object.getOwnPropertySymbols(proto).find(
      (s) => s.description === 'kResourceTypeInspect'
    );
    ok(inspect !== undefined);
    strictEqual(
      attributes(Object.getOwnPropertyDescriptor(proto, inspect)),
      'datac'
    );
  },
};

export const globalPlacement = {
  test() {
    const own = Object.getOwnPropertyDescriptor(globalThis, 'URLPattern');
    const inherited = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(globalThis),
      'URLPattern'
    );
    // Ledger #8.
    if (usingTsImpl) {
      strictEqual(attributes(own), 'datawc');
      strictEqual(inherited, undefined);
    } else {
      strictEqual(attributes(inherited), 'datawec');
      if (specCompliantPropertyAttributes) {
        strictEqual(attributes(own), 'datawc');
      } else {
        strictEqual(own, undefined);
      }
    }
  },
};
