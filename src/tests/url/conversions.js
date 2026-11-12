// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Argument conversion: order, Symbols, objects, and lone surrogates, which
// a USVString conversion replaces with U+FFFD (ledger #17).

import { throws, strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl, urlStandard } from 'which-impl';
import { kLoneSurrogate } from 'helpers';

// Whether arguments are USVStrings. The legacy C++ takes them as UTF-8,
// which keeps a lone surrogate as its WTF-8 bytes.
const usvStrings = urlStandard || usingTsImpl;
const kEncodedSurrogate = usvStrings ? '%EF%BF%BD' : '%ED%A0%80';

function logged(log, name, value) {
  return {
    toString() {
      log.push(name);
      return value;
    },
  };
}

export const conversionOrder = {
  test() {
    const log = [];
    new URL(logged(log, 'url', '/x'), logged(log, 'base', 'http://a/'));
    deepStrictEqual(log, ['url', 'base']);

    const params = new URLSearchParams();
    log.length = 0;
    params.append(logged(log, 'name', 'n'), logged(log, 'value', 'v'));
    params.set(logged(log, 'name', 'n'), logged(log, 'value', 'w'));
    deepStrictEqual(log, ['name', 'value', 'name', 'value']);
    strictEqual(params.toString(), 'n=w');

    log.length = 0;
    new URLSearchParams([[logged(log, 'k', 'k'), logged(log, 'v', 'v')]]);
    deepStrictEqual(log, ['k', 'v']);
  },
};

export const symbolsThrow = {
  test() {
    throws(() => new URL(Symbol()), TypeError);
    throws(() => new URL('/x', Symbol()), TypeError);
    throws(() => new URLSearchParams().append(Symbol(), 'v'), TypeError);
    throws(() => new URLSearchParams().get(Symbol()), TypeError);
    throws(() => new URLSearchParams([[Symbol(), 'v']]), TypeError);
    throws(() => new URLSearchParams({ a: Symbol() }), TypeError);
  },
};

export const loneSurrogates = {
  test() {
    const url = new URL(`http://a/${kLoneSurrogate}?${kLoneSurrogate}`);
    strictEqual(url.pathname, `/${kEncodedSurrogate}`);
    strictEqual(url.search, `?${kEncodedSurrogate}`);

    const params = new URLSearchParams();
    params.append(kLoneSurrogate, `x${kLoneSurrogate}`);
    strictEqual(
      params.toString(),
      `${kEncodedSurrogate}=x${kEncodedSurrogate}`
    );
    if (usvStrings) {
      strictEqual(params.get('\uFFFD'), 'x\uFFFD');
      strictEqual(params.get(kLoneSurrogate), 'x\uFFFD');
    }
  },
};

export const recordKeysAreUsvStrings = {
  test() {
    // Two keys that convert to the same USVString are one entry, at the
    // first one's position, with the last one's value.
    const params = new URLSearchParams({
      [kLoneSurrogate]: '1',
      a: '2',
      '\uFFFD': '3',
    });
    if (usvStrings) {
      deepStrictEqual(
        [...params],
        [
          ['\uFFFD', '3'],
          ['a', '2'],
        ]
      );
    } else {
      strictEqual(params.size, 3);
    }
  },
};
