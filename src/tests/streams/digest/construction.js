// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Constructor arguments: algorithm names (string or {name} object, matched
// case-insensitively over the whole name) and the options bag.

import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { digestOf } from 'digest-vectors';

export const algorithmNamesAreCaseInsensitive = {
  async test() {
    const variants = {
      crc32: ['crc32', 'CRC32', 'Crc32', 'cRc32'],
      crc32c: ['crc32c', 'CRC32C', 'Crc32c', 'crc32C', 'cRc32C'],
      crc64nvme: ['crc64nvme', 'CRC64NVME', 'Crc64Nvme', 'crc64NVME'],
      md5: ['md5', 'MD5', 'Md5'],
      'sha-256': ['sha-256', 'SHA-256', 'Sha-256'],
    };
    for (const [canonical, names] of Object.entries(variants)) {
      const expected = await digestOf(canonical, 'hello');
      for (const name of names) {
        deepStrictEqual(
          await digestOf(name, 'hello'),
          expected,
          `${name} should select the same algorithm as ${canonical}`
        );
      }
    }
  },
};

export const crcNamesStillRequireAnExactSpelling = {
  test() {
    // Case-insensitivity must not turn a nonsense name into a match: the
    // comparison is over the whole name.
    for (const name of [
      'crc',
      'crc3',
      'crc322',
      'crc32d',
      ' crc32',
      'crc32 ',
      'crc-32',
      'crc64',
      'crc64nvm',
      'crc64nvmex',
      'nvme',
    ]) {
      throws(
        () => new crypto.DigestStream(name),
        { name: 'NotSupportedError' },
        `${JSON.stringify(name)} should not be a valid algorithm`
      );
    }
  },
};

export const unknownAlgorithmThrowsSynchronously = {
  test() {
    throws(() => new crypto.DigestStream('foo'));
    throws(() => new crypto.DigestStream(''));
    // A missing name coerces to "undefined" and then fails the lookup.
    throws(() => new crypto.DigestStream({}));
    throws(() => new crypto.DigestStream(null));
    throws(() => new crypto.DigestStream(undefined));
  },
};

export const algorithmAsObject = {
  async test() {
    const check = new Uint8Array([
      93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
    ]);
    deepStrictEqual(await digestOf({ name: 'md5' }, 'hello'), check);
  },
};

export const allAlgorithms = {
  async test() {
    const sizes = {
      md5: 16,
      'SHA-1': 20,
      'SHA-256': 32,
      'SHA-384': 48,
      'SHA-512': 64,
      crc32: 4,
      crc32c: 4,
      crc64nvme: 8,
    };
    for (const [name, size] of Object.entries(sizes)) {
      strictEqual(
        (await digestOf(name, 'abc')).byteLength,
        size,
        `${name} digest size`
      );
    }
  },
};

// The option bag follows Web IDL dictionary rules: undefined and null are an
// empty bag, any object is read for its fields, and a primitive is a
// TypeError. Arrays and functions count as objects.
export const optionBagAcceptsObjectsAndRejectsPrimitives = {
  test() {
    for (const options of [undefined, null, {}, [], () => {}, new Date()]) {
      const stream = new crypto.DigestStream('md5', options);
      stream[Symbol.dispose]();
    }
    for (const options of [0, 1, '', 'x', true, false, 1n, Symbol.iterator]) {
      throws(
        () => new crypto.DigestStream('md5', options),
        {
          name: 'TypeError',
          message:
            "Failed to construct 'DigestStream': constructor parameter 2 is " +
            "not of type 'Options'.",
        },
        `should reject primitive option bag: ${String(options)}`
      );
    }
  },
};

// Argument type-checking happens before the algorithm name is looked up.
export const argumentTypesAreCheckedBeforeAlgorithmLookup = {
  test() {
    throws(() => new crypto.DigestStream('foo', 0), {
      name: 'TypeError',
      message:
        "Failed to construct 'DigestStream': constructor parameter 2 is not " +
        "of type 'Options'.",
    });
    throws(() => new crypto.DigestStream('foo', { toWellFormed: true }), {
      name: 'NotSupportedError',
    });
  },
};
