// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What enumerating an instance sees, which is what logging code that
// serializes errors as objects sees (ledger #1 and #2).

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { kLegacyCodes } from 'helpers';

export const jsonStringify = {
  test() {
    const error = new DOMException('m', 'AbortError');
    const expected = usingTsImpl
      ? {}
      : { code: 20, name: 'AbortError', message: 'm', stack: error.stack };
    deepStrictEqual(JSON.parse(JSON.stringify(error)), expected);
  },
};

export const enumerableKeys = {
  test() {
    const error = new DOMException('m', 'AbortError');
    const expected = usingTsImpl ? [] : ['code', 'name', 'message', 'stack'];
    deepStrictEqual(Object.keys(error), expected);
    deepStrictEqual(Object.keys({ ...error }), expected);
    deepStrictEqual(Object.keys(Object.assign({}, error)), expected);
    // for-in also visits the prototype's enumerable properties: the
    // constants and, in WebIDL, the attributes.
    const forIn = [];
    for (const key in error) forIn.push(key);
    const inherited = kLegacyCodes.map(([constant]) => constant);
    if (usingTsImpl) inherited.push('code', 'message', 'name');
    deepStrictEqual(forIn.sort(), [...expected, ...inherited].sort());
  },
};

export const addedPropertiesEnumerate = {
  test() {
    const error = new DOMException('m', 'AbortError');
    error.detail = 1;
    strictEqual(Object.keys(error).at(-1), 'detail');
    strictEqual(JSON.parse(JSON.stringify(error)).detail, 1);
  },
};
