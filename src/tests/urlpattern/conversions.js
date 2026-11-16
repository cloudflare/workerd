// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// WebIDL conversions of the arguments: the URLPatternInit and
// URLPatternOptions dictionaries, and USVStrings.

import { throws, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, urlPatternStandard } from 'which-impl';
import { kComponents, kInitMembers, kLoneSurrogate } from 'helpers';

// An init whose every member logs its read.
function loggingInit(log) {
  const init = {};
  for (const name of [...kComponents, 'baseURL']) {
    Object.defineProperty(init, name, {
      get() {
        log.push(name);
        return undefined;
      },
      enumerable: true,
    });
  }
  return init;
}

export const initReadOrder = {
  test() {
    // Ledger #3.
    const expected = usingTsImpl ? kInitMembers : [...kComponents, 'baseURL'];
    for (const use of [
      (init) => new URLPattern(init),
      (init) => new URLPattern({}).exec(init),
      (init) => new URLPattern({}).test(init),
    ]) {
      const log = [];
      use(loggingInit(log));
      deepStrictEqual(log, expected);
    }
  },
};

export const initMembersAreStrings = {
  test() {
    const pattern = new URLPattern({
      pathname: { toString: () => '/a' },
      search: null,
      hash: undefined,
    });
    strictEqual(pattern.pathname, '/a');
    strictEqual(pattern.search, 'null');
    strictEqual(pattern.hash, '*');
    throws(() => new URLPattern({ pathname: Symbol() }), {
      name: 'TypeError',
    });
    throws(() => new URLPattern({}).exec({ hash: Symbol() }), {
      name: 'TypeError',
    });
    const thrown = new Error('boom');
    throws(
      () =>
        new URLPattern({
          get pathname() {
            throw thrown;
          },
        }),
      (e) => e === thrown
    );
  },
};

export const argumentConversionOrder = {
  test() {
    const log = [];
    const string = (name, value) => ({
      toString() {
        log.push(name);
        return value;
      },
    });
    // Objects are inits, so only a primitive input converts as a string.
    new URLPattern({}).exec('/x', string('base', 'https://a/'));
    const make = () =>
      new URLPattern('/x', string('base2', 'https://a/'), {
        get ignoreCase() {
          log.push('ignoreCase');
          return false;
        },
      });
    // Ledger #7: the C++ takes an object second argument as the options,
    // reads the third and ignores it, so the relative pattern has no
    // baseURL.
    if (usingTsImpl) {
      make();
      deepStrictEqual(log, ['base', 'base2', 'ignoreCase']);
    } else {
      throws(make, { name: 'TypeError' });
      deepStrictEqual(log, ['base', 'ignoreCase']);
    }
  },
};

export const optionsReadOnlyIgnoreCase = {
  test() {
    const log = [];
    const options = new Proxy(
      {},
      {
        get(target, key) {
          log.push(String(key));
          return undefined;
        },
      }
    );
    new URLPattern({}, options);
    deepStrictEqual(log, ['ignoreCase']);
  },
};

export const loneSurrogates = {
  test() {
    // Ledger #6: the legacy C++ keeps WTF-8 bytes; USVStrings replace the
    // surrogate with U+FFFD, which the pattern percent-encodes.
    const usv = urlPatternStandard || usingTsImpl;
    const replacement = '/%EF%BF%BD';
    strictEqual(
      new URLPattern({ pathname: `/${kLoneSurrogate}` }).pathname,
      usv ? replacement : '/%ED%A0%80'
    );
    strictEqual(
      new URLPattern({ pathname: replacement }).test({
        pathname: `/${kLoneSurrogate}`,
      }),
      usv
    );
    strictEqual(
      new URLPattern({ pathname: replacement }).test(
        `https://a/${kLoneSurrogate}`
      ),
      usv
    );
  },
};
