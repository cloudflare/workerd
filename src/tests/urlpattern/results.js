// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// exec()'s results: their shape, their groups and their inputs. Every
// call makes new objects.

import { strictEqual, deepStrictEqual, ok, notStrictEqual } from 'node:assert';
import { usingTsImpl, urlPatternStandard, fastJsgStruct } from 'which-impl';
import { kComponents, kInitMembers } from 'helpers';

// Ledger #4: WebIDL dictionaries' members are in lexicographic order.
const kResultMembers = usingTsImpl
  ? [
      'hash',
      'hostname',
      'inputs',
      'password',
      'pathname',
      'port',
      'protocol',
      'search',
      'username',
    ]
  : ['inputs', ...kComponents];
const kComponentResultMembers = usingTsImpl
  ? ['groups', 'input']
  : ['input', 'groups'];

export const resultShape = {
  test() {
    const pattern = new URLPattern({ pathname: '/:a' });
    const result = pattern.exec('https://h/x');
    deepStrictEqual(Object.keys(result), kResultMembers);
    strictEqual(Object.getPrototypeOf(result), Object.prototype);
    for (const name of kComponents) {
      const component = result[name];
      deepStrictEqual(Object.keys(component), kComponentResultMembers, name);
      strictEqual(Object.getPrototypeOf(component), Object.prototype);
      strictEqual(Object.getPrototypeOf(component.groups), Object.prototype);
      for (const key of Reflect.ownKeys(component)) {
        const d = Object.getOwnPropertyDescriptor(component, key);
        deepStrictEqual(
          [d.writable, d.enumerable, d.configurable],
          [true, true, true]
        );
      }
    }
    ok(Array.isArray(result.inputs));
    strictEqual(Object.getPrototypeOf(result.inputs), Array.prototype);
    notStrictEqual(pattern.exec('https://h/x'), result);
  },
};

export const groups = {
  test() {
    const pattern = new URLPattern({
      pathname: '/:zz/:aa/:mm/:b/:c/:d/:e',
    });
    const { groups } = pattern.exec({ pathname: '/1/2/3/4/5/6/7' }).pathname;
    const names = ['zz', 'aa', 'mm', 'b', 'c', 'd', 'e'];
    // Ledger #2: the standard C++ has ada's std::unordered_map order, which
    // is not pinned.
    if (urlPatternStandard && !usingTsImpl) {
      deepStrictEqual(Object.keys(groups).sort(), [...names].sort());
    } else {
      deepStrictEqual(Object.keys(groups), names);
    }
    deepStrictEqual(
      { ...groups },
      { zz: '1', aa: '2', mm: '3', b: '4', c: '5', d: '6', e: '7' }
    );

    // Wildcards and regexp groups are numbered, and integer keys come
    // first.
    deepStrictEqual(
      new URLPattern({ pathname: '/(\\d+)/:x/*' }).exec({ pathname: '/1/y/z' })
        .pathname.groups,
      { 0: '1', 1: 'z', x: 'y' }
    );
    // A regexp's own capturing groups are not groups of the result.
    const nested = new URLPattern({ pathname: '/:a/((?<foo>x))' }).exec({
      pathname: '/1/x',
    }).pathname.groups;
    deepStrictEqual(Object.keys(nested), ['0', 'a']);
    deepStrictEqual({ ...nested }, { 0: 'x', a: '1' });

    // Components with no groups have an empty groups object, but a
    // wildcard has one.
    const result = new URLPattern({ pathname: '/a', search: '' }).exec(
      'https://h/a'
    );
    deepStrictEqual(result.pathname.groups, {});
    deepStrictEqual(result.search.groups, {});
    deepStrictEqual(result.hostname.groups, { 0: 'h' });
  },
};

// A named group whose regexp is `.*` keeps its name, though the standard
// parser matches it without a RegExp, as it does `*`.
export const namedFullWildcard = {
  test() {
    const hostname = new URLPattern({ hostname: ':domain(.*)' });
    deepStrictEqual(hostname.exec({ hostname: 'localhost' }).hostname.groups, {
      domain: 'localhost',
    });
    deepStrictEqual(hostname.exec('https://a.b/').hostname.groups, {
      domain: 'a.b',
    });
    deepStrictEqual(
      new URLPattern({ pathname: ':rest(.*)' }).exec('https://h/a/b').pathname
        .groups,
      { rest: '/a/b' }
    );
    deepStrictEqual(
      new URLPattern({ search: ':q(.*)' }).exec({ search: '' }).search.groups,
      { q: '' }
    );
  },
};

export const unmatchedOptionalGroups = {
  test() {
    const { groups } = new URLPattern({ pathname: '/:a{/:b}?' }).exec({
      pathname: '/x',
    }).pathname;
    ok(Object.hasOwn(groups, 'b'));
    // The legacy parser makes an unmatched group the empty string.
    strictEqual(groups.b, urlPatternStandard ? undefined : '');
    strictEqual(groups.a, 'x');
  },
};

export const stringInputsAreInputs = {
  test() {
    const pattern = new URLPattern({});
    deepStrictEqual(pattern.exec('https://a/x').inputs, ['https://a/x']);
    deepStrictEqual(pattern.exec('/x', 'https://b/').inputs, [
      '/x',
      'https://b/',
    ]);
    // Converted to strings.
    deepStrictEqual(
      pattern.exec(
        { toString: () => 'unused' }.toString(),
        new URL('https://c/')
      ).inputs,
      ['unused', 'https://c/']
    );
  },
};

export const initInputsAreCopied = {
  test() {
    const pattern = new URLPattern({});
    const init = { pathname: '/x', hash: 'h', extra: 1 };
    const [copy] = pattern.exec(init).inputs;
    notStrictEqual(copy, init);
    strictEqual(Object.getPrototypeOf(copy), Object.prototype);
    // Only the members it has, in WebIDL's order (ledger #4). Ledger #11:
    // with enable_fast_jsg_struct, the C++ copy has every member, those it
    // lacks undefined.
    if (usingTsImpl) {
      deepStrictEqual(Object.keys(copy), ['hash', 'pathname']);
    } else if (fastJsgStruct) {
      deepStrictEqual(Object.keys(copy), [...kComponents, 'baseURL']);
      strictEqual(copy.protocol, undefined);
    } else {
      deepStrictEqual(Object.keys(copy), ['pathname', 'hash']);
    }
    strictEqual(copy.pathname, '/x');
    strictEqual(copy.hash, 'h');

    const all = {
      protocol: 'https',
      username: 'u',
      password: 'p',
      hostname: 'h',
      port: '1',
      pathname: '/p',
      search: 's',
      hash: 'f',
      baseURL: 'https://b/',
    };
    const [allCopy] = pattern.exec(all).inputs;
    deepStrictEqual(
      Object.keys(allCopy),
      usingTsImpl ? kInitMembers : [...kComponents, 'baseURL']
    );
    // As given, not processed.
    strictEqual(
      pattern.exec({ protocol: 'HTTP:' }).inputs[0].protocol,
      'HTTP:'
    );
    const [empty] = pattern.exec().inputs;
    deepStrictEqual(
      Object.keys(empty),
      fastJsgStruct && !usingTsImpl ? [...kComponents, 'baseURL'] : []
    );
  },
};
