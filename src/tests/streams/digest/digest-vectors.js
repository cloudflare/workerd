// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pinned digest outputs, including the AWS SDK CRC vectors. Both
// implementations drive the same native digest context, so every vector
// must agree byte-for-byte.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { Buffer } from 'node:buffer';

export async function digestOf(algorithm, chunk, options) {
  const stream = new crypto.DigestStream(algorithm, options);
  const writer = stream.getWriter();
  await writer.write(chunk);
  await writer.close();
  return new Uint8Array(await stream.digest);
}

export const md5Vectors = {
  async test() {
    const helloMd5 = new Uint8Array([
      93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
    ]);
    // The same input as bytes and as a string chunk.
    deepStrictEqual(
      await digestOf('md5', new TextEncoder().encode('hello')),
      helloMd5
    );
    deepStrictEqual(await digestOf('md5', 'hello'), helloMd5);
  },
};

export const typedArrayChunks = {
  async test() {
    const check = new Uint8Array([
      70, 54, 153, 61, 62, 29, 164, 233, 214, 184, 248, 123, 121, 232, 247, 198,
      208, 24, 88, 13, 82, 102, 25, 80, 234, 188, 56, 69, 197, 137, 122, 77,
    ]);
    // A non-Uint8 view hashes its underlying bytes.
    deepStrictEqual(
      await digestOf('SHA-256', new Uint32Array([1, 2, 3])),
      check
    );
    // A view with a byteOffset hashes only its window.
    deepStrictEqual(
      await digestOf('SHA-256', new Uint32Array([0, 1, 2, 3]).subarray(1)),
      check
    );
    deepStrictEqual(
      await digestOf('crc32', new Uint32Array([1, 2, 3])),
      new Uint8Array([176, 224, 34, 147])
    );
  },
};

// Source:
// https://github.com/aws/aws-sdk-js-v3/blob/c3f3d0a1c652c88fef5859881c9a12cfc8df61c1/packages/middleware-flexible-checksums/src/middleware-flexible-checksums.integ.spec.ts#L21
export const awsChecksumVectors = {
  async test() {
    const testCases = [
      ['', 'crc32', 'AAAAAA=='],
      ['abc', 'crc32', 'NSRBwg=='],
      ['Hello world', 'crc32', 'i9aeUg=='],

      ['', 'crc32c', 'AAAAAA=='],
      ['abc', 'crc32c', 'Nks/tw=='],
      ['Hello world', 'crc32c', 'crUfeA=='],

      ['', 'crc64nvme', 'AAAAAAAAAAA='],
      ['abc', 'crc64nvme', 'BeXKuz/B+us='],
      ['Hello world', 'crc64nvme', 'OOJZ0D8xKts='],

      ['', 'SHA-1', '2jmj7l5rSw0yVb/vlWAYkK/YBwk='],
      ['abc', 'SHA-1', 'qZk+NkcGgWq6PiVxeFDCbJzQ2J0='],
      ['Hello world', 'SHA-1', 'e1AsOh9IyGCa4hLN+2Od7jlnP14='],

      ['', 'SHA-256', '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU='],
      ['abc', 'SHA-256', 'ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0='],
      [
        'Hello world',
        'SHA-256',
        'ZOyIygCyaOW6GjVnihtTFtIS9PNmskdyMlNKiuyjfzw=',
      ],
    ];
    for (const [body, algorithm, expected] of testCases) {
      const digest = await digestOf(algorithm, new TextEncoder().encode(body));
      deepStrictEqual(
        digest,
        new Uint8Array(Buffer.from(expected, 'base64')),
        `${algorithm} of ${JSON.stringify(body)}`
      );
    }
  },
};

export const multipleWritesAccumulate = {
  async test() {
    const check = new Uint8Array([
      198, 247, 195, 114, 100, 29, 210, 94, 15, 221, 240, 33, 83, 117, 86, 31,
    ]);
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();
    // Mixed input kinds must concatenate in write order.
    await writer.write(enc.encode('hel'));
    await writer.write('lo');
    await writer.write(new Uint8Array(0));
    await writer.write(enc.encode('there'));
    await writer.close();
    deepStrictEqual(new Uint8Array(await stream.digest), check);
    strictEqual(stream.bytesWritten, 10n);
  },
};
