// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

'use strict';

import {
  strictEqual,
  throws,
  rejects,
} from 'node:assert';

import {
  scrypt,
  scryptSync,
} from 'node:crypto';

import {
  mock,
} from 'node:test';

const good = [
  // Zero-length key is legal, functions as a parameter validation check.
  {
    pass: '',
    salt: '',
    keylen: 0,
    N: 16,
    p: 1,
    r: 1,
    expected: '',
  },
  // Test vectors from https://tools.ietf.org/html/rfc7914#page-13 that
  // should pass.  Note that the test vector with N=1048576 is omitted
  // because it takes too long to complete and uses over 1 GiB of memory.
  {
    pass: '',
    salt: '',
    keylen: 64,
    N: 16,
    p: 1,
    r: 1,
    expected:
        '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442' +
        'fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906',
  },
  {
    pass: 'password',
    salt: 'NaCl',
    keylen: 64,
    N: 1024,
    p: 16,
    r: 8,
    expected:
        'fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162' +
        '2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640',
  },
  {
    pass: 'pleaseletmein',
    salt: 'SodiumChloride',
    keylen: 64,
    N: 16384,
    p: 1,
    r: 8,
    expected:
        '7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2' +
        'd5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887',
  },
  {
    pass: '',
    salt: '',
    keylen: 64,
    cost: 16,
    parallelization: 1,
    blockSize: 1,
    expected:
        '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442' +
        'fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906',
  },
  {
    pass: 'password',
    salt: 'NaCl',
    keylen: 64,
    cost: 1024,
    parallelization: 16,
    blockSize: 8,
    expected:
        'fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162' +
        '2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640',
  },
  {
    pass: 'pleaseletmein',
    salt: 'SodiumChloride',
    keylen: 64,
    cost: 16384,
    parallelization: 1,
    blockSize: 8,
    expected:
        '7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2' +
        'd5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887',
  },
];

// Test vectors that should fail.
const bad = [
  { N: 1, p: 1, r: 1 },         // N < 2
  { N: 3, p: 1, r: 1 },         // Not power of 2.
];

// Test vectors where 128*N*r exceeds maxmem.
const toobig = [
  { N: 2 ** 16, p: 1, r: 1 },   // N >= 2**(r*16)
  { N: 2, p: 2 ** 30, r: 1 },   // p > (2**30-1)/r
  { N: 2 ** 20, p: 1, r: 8 },
  { N: 2 ** 10, p: 1, r: 8, maxmem: 2 ** 20 },
];

const badargs = [
  {
    args: [],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"password"/ },
  },
  {
    args: [null],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"password"/ },
  },
  {
    args: [''],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"salt"/ },
  },
  {
    args: ['', null],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"salt"/ },
  },
  {
    args: ['', ''],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"keylen"/ },
  },
  {
    args: ['', '', null],
    expected: { code: 'ERR_INVALID_ARG_TYPE', message: /"keylen"/ },
  },
  {
    args: ['', '', .42],
    expected: { code: 'ERR_OUT_OF_RANGE', message: /"keylen"/ },
  },
  {
    args: ['', '', -42],
    expected: { code: 'ERR_OUT_OF_RANGE', message: /"keylen"/ },
  },
  {
    args: ['', '', 2 ** 31],
    expected: { code: 'ERR_OUT_OF_RANGE', message: /"keylen"/ },
  },
  {
    args: ['', '', 2147485780],
    expected: { code: 'ERR_OUT_OF_RANGE', message: /"keylen"/ },
  },
  {
    args: ['', '', 2 ** 32],
    expected: { code: 'ERR_OUT_OF_RANGE', message: /"keylen"/ },
  },
];

export const goodTests = {
  async test() {
    for (const options of good) {
      const { pass, salt, keylen, expected } = options;
      const actual = scryptSync(pass, salt, keylen, options);
      strictEqual(actual.toString('hex'), expected);
      const { promise, resolve } = Promise.withResolvers();
      const fn = mock.fn((err, actual) => {
        strictEqual(actual.toString('hex'), expected);
        resolve();
      });
      scrypt(pass, salt, keylen, options, fn);
      await promise;
      strictEqual(fn.mock.calls.length, 1);
    }
  }
}

export const badTests = {
  async test() {
    for (const options of bad) {
      const { promise, reject } = Promise.withResolvers();
      const fn = mock.fn((err, actual) => {
        if (err) reject(err);
      });
      scrypt('pass', 'salt', 1, options, fn);
      await rejects(promise);
      throws(() => scryptSync('pass', 'salt', 1, options));
    }
    throws(() => scryptSync('pass', 'salt', 1, { N: 1, cost: 1 }));
    throws(() => scryptSync('pass', 'salt', 1, { p: 1, parallelization: 1 }));
    throws(() => scryptSync('pass', 'salt', 1, { r: 1, blockSize: 1 }));
  }
};

export const tooBigTests = {
  async test() {
  for (const options of toobig) {
    const { promise, reject } = Promise.withResolvers();
    const fn = mock.fn((err, actual) => {
      if (err) reject(err);
    });
    scrypt('pass', 'salt', 1, options, fn);
    await rejects(promise);
    strictEqual(fn.mock.calls.length, 1);

    throws(() => scryptSync('pass', 'salt', 1, options));
  }
  }
};

export const defaultsTest = {
  async test() {
    const defaults = { N: 16384, p: 1, r: 8 };
    const expected = scryptSync('pass', 'salt', 1, defaults);
    const actual = scryptSync('pass', 'salt', 1);
    strictEqual(actual.toString('hex'), expected.toString('hex'));
    const { promise, resolve } = Promise.withResolvers();
    const fn = mock.fn((err, actual) => {
      strictEqual(actual.toString('hex'), expected.toString('hex'));
      resolve();
    });
    scrypt('pass', 'salt', 1, fn);
    await promise;
    strictEqual(fn.mock.calls.length, 1);
  }
};

export const badArgsTest = {
  test() {
    for (const { args, expected } of badargs) {
      throws(() => scrypt(...args));
      throws(() => scryptSync(...args));
    }

    throws(() => scrypt('', '', 42, null));
    throws(() => scrypt('', '', 42, {}, null));
    throws(() => scrypt('', '', 42, {}));
    throws(() => scrypt('', '', 42, {}, {}));
  }
}
