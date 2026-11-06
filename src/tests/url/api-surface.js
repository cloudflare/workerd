// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The shape of URL and URLSearchParams: members and their order,
// attributes, names and lengths, statics, Symbol.toStringTag, the runtime
// class marks and the globals' placement.

import { deepStrictEqual, strictEqual, ok } from 'node:assert';
import {
  usingTsImpl,
  urlStandard,
  gettersSettersOnPrototype,
  setToStringTag,
  webIdlAttributes,
  nonWritablePrototype,
  specCompliantPropertyAttributes,
} from 'which-impl';
import {
  attributes,
  memberNames,
  kUrlAttributes,
  kReadonlyUrlAttributes,
  kSearchParamsMethods,
} from 'helpers';

const instanceLayout = !urlStandard && !gettersSettersOnPrototype;

// The legacy URL's own instance properties, in the order JSG defines them.
const kInstanceOrder = [...kUrlAttributes].reverse();

export const urlPrototypeMembers = {
  test() {
    let expected;
    if (usingTsImpl) {
      // WebIDL: the attributes, the operations, then the stringifier (ledger
      // #5), whatever the backend and layout (ledger #22).
      expected = [...kUrlAttributes, 'toJSON', 'toString'];
    } else if (urlStandard) {
      expected = [
        'origin',
        'href',
        'protocol',
        'username',
        'password',
        'host',
        'hostname',
        'port',
        'pathname',
        'search',
        'hash',
        'searchParams',
        'toJSON',
        'toString',
      ];
    } else if (instanceLayout) {
      expected = ['toString', 'toJSON'];
    } else {
      expected = [...kUrlAttributes, 'toString', 'toJSON'];
    }
    deepStrictEqual(memberNames(URL.prototype), expected);
  },
};

export const urlMemberAttributes = {
  test() {
    const proto = URL.prototype;
    for (const name of ['toString', 'toJSON']) {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(d), 'datawec', name);
      strictEqual(d.value.name, name);
      strictEqual(d.value.length, 0, name);
    }
    if (instanceLayout && !usingTsImpl) return;
    for (const name of kUrlAttributes) {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(d), 'accessorec', name);
      const readonly = kReadonlyUrlAttributes.includes(name);
      strictEqual(d.set === undefined, readonly, name);
      // Ledger #13.
      strictEqual(d.get.name, webIdlAttributes ? `get ${name}` : '', name);
      strictEqual(d.get.length, 0, name);
      if (!readonly) {
        strictEqual(d.set.name, webIdlAttributes ? `set ${name}` : '', name);
        strictEqual(d.set.length, webIdlAttributes ? 1 : 0, name);
      }
    }
  },
};

export const urlStatics = {
  test() {
    const statics = ['canParse', 'parse', 'createObjectURL', 'revokeObjectURL'];
    if (!urlStandard && !usingTsImpl) {
      // Ledger #15.
      for (const name of statics) strictEqual(URL[name], undefined, name);
      return;
    }
    for (const name of statics) {
      const d = Object.getOwnPropertyDescriptor(URL, name);
      strictEqual(attributes(d), 'datawec', name);
      strictEqual(d.value.name, name);
      strictEqual(d.value.length, webIdlAttributes ? 1 : 0, name);
    }
  },
};

export const searchParamsPrototypeMembers = {
  test() {
    const proto = URLSearchParams.prototype;
    deepStrictEqual(memberNames(proto), ['size', ...kSearchParamsMethods]);
    const size = Object.getOwnPropertyDescriptor(proto, 'size');
    strictEqual(attributes(size), 'accessorec');
    strictEqual(size.set, undefined);
    strictEqual(size.get.name, webIdlAttributes ? 'get size' : '');

    const lengths = {
      append: 2,
      delete: 1,
      get: 1,
      getAll: 1,
      has: 1,
      set: 2,
      sort: 0,
      entries: 0,
      keys: 0,
      values: 0,
      forEach: 1,
      toString: 0,
    };
    for (const name of kSearchParamsMethods) {
      const d = Object.getOwnPropertyDescriptor(proto, name);
      strictEqual(attributes(d), 'datawec', name);
      strictEqual(d.value.name, name);
      strictEqual(d.value.length, webIdlAttributes ? lengths[name] : 0, name);
    }

    const iterator = Object.getOwnPropertyDescriptor(proto, Symbol.iterator);
    strictEqual(attributes(iterator), 'datawc');
    // Ledger #6.
    strictEqual(iterator.value === proto.entries, usingTsImpl);
  },
};

export const constructorAttributes = {
  test() {
    for (const [ctor, name, length] of [
      [URL, 'URL', 1],
      [URLSearchParams, 'URLSearchParams', 0],
    ]) {
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'name')),
        'datac'
      );
      strictEqual(ctor.name, name);
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'length')),
        'datac'
      );
      // Ledger #13.
      strictEqual(ctor.length, webIdlAttributes ? length : 0, name);
      // Ledger #2.
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(ctor, 'prototype')),
        nonWritablePrototype ? 'data' : 'dataw',
        name
      );
      const c = Object.getOwnPropertyDescriptor(ctor.prototype, 'constructor');
      strictEqual(attributes(c), 'datawc', name);
      strictEqual(c.value, ctor);
    }
  },
};

// Python Workers accept a Response body by its constructor's name, and
// node:url's urlToHttpOptions() spreads a URL.
export const constructorNamesAndSpread = {
  test() {
    strictEqual(new URL('http://a/').constructor.name, 'URL');
    strictEqual(new URLSearchParams().constructor.name, 'URLSearchParams');
    deepStrictEqual(
      Object.keys({ ...new URL('http://a/?q') }),
      instanceLayout ? kInstanceOrder : []
    );
    deepStrictEqual(Reflect.ownKeys(new URLSearchParams('a=1')), []);
  },
};

export const legacyInstanceProperties = {
  test() {
    const url = new URL('http://a/');
    if (!instanceLayout) {
      deepStrictEqual(Reflect.ownKeys(url), []);
      return;
    }
    deepStrictEqual(Reflect.ownKeys(url), kInstanceOrder);
    for (const name of kInstanceOrder) {
      const d = Object.getOwnPropertyDescriptor(url, name);
      const readonly = kReadonlyUrlAttributes.includes(name);
      // Ledger #1: JSG's are native data properties; JavaScript can only
      // make accessors.
      if (usingTsImpl) {
        strictEqual(attributes(d), 'accessorec', name);
        strictEqual(d.set === undefined, readonly, name);
      } else {
        strictEqual(attributes(d), readonly ? 'dataec' : 'datawec', name);
      }
    }
  },
};

export const legacyInstancePropertiesShadowSubclasses = {
  test() {
    class Sub extends URL {
      get href() {
        return 'override';
      }
    }
    const url = new Sub('http://a/');
    ok(url instanceof URL);
    strictEqual(url.href, instanceLayout ? 'http://a/' : 'override');
    strictEqual(String(url), 'http://a/');
    strictEqual(Object.keys(url).length, instanceLayout ? 12 : 0);
  },
};

export const toStringTagBranding = {
  test() {
    const tag = (v) => Object.prototype.toString.call(v);
    const params = new URLSearchParams('a=1');
    if (!setToStringTag) {
      strictEqual(tag(new URL('http://a/')), '[object Object]');
      strictEqual(tag(params), '[object Object]');
      strictEqual(tag(params.entries()), '[object Iterator]');
      for (const proto of [URL.prototype, URLSearchParams.prototype]) {
        ok(!Object.hasOwn(proto, Symbol.toStringTag));
      }
      return;
    }
    strictEqual(tag(new URL('http://a/')), '[object URL]');
    strictEqual(tag(params), '[object URLSearchParams]');
    for (const [proto, name] of [
      [URL.prototype, 'URL'],
      [URLSearchParams.prototype, 'URLSearchParams'],
    ]) {
      const d = Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag);
      strictEqual(d.value, name);
      // Ledger #20.
      strictEqual(attributes(d), usingTsImpl ? 'datac' : 'datawc');
    }
    // Ledger #6.
    if (usingTsImpl) {
      for (const it of [params.entries(), params.keys(), params.values()]) {
        strictEqual(tag(it), '[object URLSearchParams Iterator]');
      }
    } else {
      strictEqual(tag(params.entries()), '[object EntryIterator]');
      strictEqual(tag(params.keys()), '[object KeyIterator]');
      strictEqual(tag(params.values()), '[object ValueIterator]');
    }
  },
};

export const runtimeClassMarks = {
  test() {
    const kInternalClass = Symbol.for('cloudflare:internal-class');
    for (const proto of [URL.prototype, URLSearchParams.prototype]) {
      const d = Object.getOwnPropertyDescriptor(proto, kInternalClass);
      strictEqual(attributes(d), 'data');
      const inspect = Object.getOwnPropertySymbols(proto).find(
        (s) => s.description === 'kResourceTypeInspect'
      );
      ok(inspect !== undefined);
      strictEqual(
        attributes(Object.getOwnPropertyDescriptor(proto, inspect)),
        'datac'
      );
    }
  },
};

export const globalsPlacement = {
  test() {
    const globalProto = Object.getPrototypeOf(globalThis);
    for (const name of ['URL', 'URLSearchParams']) {
      const own = Object.getOwnPropertyDescriptor(globalThis, name);
      const inherited = Object.getOwnPropertyDescriptor(globalProto, name);
      // Ledger #14.
      if (usingTsImpl) {
        strictEqual(attributes(own), 'datawc', name);
        strictEqual(inherited, undefined, name);
      } else {
        strictEqual(attributes(inherited), 'datawec', name);
        if (specCompliantPropertyAttributes) {
          strictEqual(attributes(own), 'datawc', name);
        } else {
          strictEqual(own, undefined, name);
        }
      }
      strictEqual(globalThis[name].name, name);
    }
  },
};

export const nodeUrlHandsOutTheGlobals = {
  async test() {
    const nodeUrl = await import('node:url');
    strictEqual(nodeUrl.URL, URL);
    strictEqual(nodeUrl.URLSearchParams, URLSearchParams);
  },
};
