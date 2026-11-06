// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What user changes to builtins reach: RegExp.prototype.exec, and setters
// on Object.prototype and Array.prototype named like the results'
// members.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';
import { usingTsImpl, urlPatternStandard, fastJsgStruct } from 'which-impl';
import { kComponents } from 'helpers';

function withSetters(proto, names, f) {
  const log = [];
  for (const name of names) {
    Object.defineProperty(proto, name, {
      set() {
        log.push(name);
      },
      configurable: true,
    });
  }
  try {
    return [f(), log];
  } finally {
    for (const name of names) Reflect.deleteProperty(proto, name);
  }
}

export const regExpExecIsNotCalled = {
  test() {
    const exec = RegExp.prototype.exec;
    let calls = 0;
    RegExp.prototype.exec = function (...args) {
      calls++;
      return exec.apply(this, args);
    };
    const counts = [];
    try {
      const pattern = new URLPattern({ pathname: '/:a', hostname: 'x' });
      ok(pattern.exec({ pathname: '/y', hostname: 'x' }) !== null);
      counts.push(calls);
      ok(pattern.test({ pathname: '/y', hostname: 'x' }));
      counts.push(calls);
      ok(pattern.test('https://x/y'));
      counts.push(calls);
    } finally {
      RegExp.prototype.exec = exec;
    }
    // Ledger #5: the C++ calls it for each component its fast paths do not
    // cover, which for the legacy parser is all eight.
    let expected;
    if (usingTsImpl) expected = [0, 0, 0];
    else if (urlPatternStandard) expected = [1, 2, 3];
    else expected = [8, 16, 24];
    deepStrictEqual(counts, expected);
  },
};

export const groupsIgnoreObjectPrototypeSetters = {
  test() {
    // One named group, four (more than the implementation writes as a
    // literal), and a wildcard's numbered group, which every component of
    // that pattern has. The C++ sets each group, so the last element is
    // the number of setter calls it makes. A counter, not a log
    // array: a '0' setter on Object.prototype would take the log's pushes.
    for (const [pattern, input, names, cppCalls] of [
      [{ pathname: '/:a' }, { pathname: '/x' }, ['a'], 1],
      [
        { pathname: '/:a/:b/:c/:d' },
        { pathname: '/1/2/3/4' },
        ['a', 'b', 'c', 'd'],
        4,
      ],
      [{ pathname: '*' }, { pathname: '/x' }, ['0'], 8],
    ]) {
      let calls = 0;
      for (const name of names) {
        Object.defineProperty(Object.prototype, name, {
          set() {
            calls++;
          },
          configurable: true,
        });
      }
      let groups;
      try {
        groups = new URLPattern(pattern).exec(input).pathname.groups;
      } finally {
        for (const name of names) {
          Reflect.deleteProperty(Object.prototype, name);
        }
      }
      // Ledger #10.
      for (const name of names) {
        strictEqual(Object.hasOwn(groups, name), usingTsImpl, name);
      }
      strictEqual(calls, usingTsImpl ? 0 : cppCalls);
    }
  },
};

export const resultsIgnoreObjectPrototypeSetters = {
  test() {
    const [result, log] = withSetters(
      Object.prototype,
      ['pathname', 'inputs', 'input', 'groups'],
      () => new URLPattern({ pathname: '/:a' }).exec({ pathname: '/x' })
    );
    // Ledger #10: without enable_fast_jsg_struct, the C++ assigns each
    // member of a result.
    if (usingTsImpl || fastJsgStruct) {
      deepStrictEqual(log, []);
      ok(Object.hasOwn(result, 'pathname'));
      ok(Object.hasOwn(result.pathname, 'groups'));
      ok(Object.hasOwn(result.inputs[0], 'pathname'));
    } else {
      // The init's copy's pathname, inputs, each component's input and
      // groups, then pathname.
      strictEqual(log.length, 2 + 2 * kComponents.length + 1);
      ok(!Object.hasOwn(result, 'pathname'));
      ok(!Object.hasOwn(result, 'inputs'));
      ok(!Object.hasOwn(result.hash, 'groups'));
    }
  },
};

export const inputsIgnoreArrayPrototypeSetters = {
  test() {
    const [result, log] = withSetters(Array.prototype, ['0', '1'], () =>
      new URLPattern({}).exec('/x', 'https://a/')
    );
    deepStrictEqual(log, []);
    deepStrictEqual(result.inputs, ['/x', 'https://a/']);
  },
};
