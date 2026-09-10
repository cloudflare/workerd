// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Without workers_api_getters_setters_on_prototype the digest promise is a
// per-instance own property (no prototype accessor exists); without
// set_tostring_tag instances stringify as [object Object]. bytesWritten
// remains a prototype accessor returning a bigint in every era, and the
// digest flow itself is unchanged.

import { strictEqual, deepStrictEqual } from 'node:assert';

export const legacyToStringTag = {
  test() {
    strictEqual(
      Object.prototype.toString.call(new crypto.DigestStream('md5')),
      '[object Object]'
    );
  },
};

export const legacyDigestIsOwnInstanceProperty = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    deepStrictEqual(Object.getOwnPropertyNames(stream).toSorted(), [
      'digest',
      'locked',
    ]);
    strictEqual(
      Object.getOwnPropertyDescriptor(crypto.DigestStream.prototype, 'digest'),
      undefined
    );
    // Still the working digest promise, and bytesWritten stays a prototype
    // accessor returning a bigint.
    strictEqual(stream.digest.constructor, Promise);
    strictEqual(typeof stream.bytesWritten, 'bigint');
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    strictEqual((await stream.digest).byteLength, 16);
    strictEqual(stream.bytesWritten, 5n);
  },
};
