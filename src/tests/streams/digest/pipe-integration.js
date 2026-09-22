// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// DigestStream as a pipe destination: pipeTo brand checks pass and content
// digests correctly from user streams, transforms, and Response bodies.

import { strictEqual, deepStrictEqual } from 'node:assert';

const helloMd5 = new Uint8Array([
  93, 65, 64, 42, 188, 75, 42, 118, 185, 113, 157, 145, 16, 23, 197, 146,
]);

export const pipeToWorks = {
  async test() {
    const enc = new TextEncoder();
    const digestStream = new crypto.DigestStream('md5');
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('hello'));
        controller.close();
      },
    });
    await source.pipeTo(digestStream);
    deepStrictEqual(new Uint8Array(await digestStream.digest), helloMd5);
    strictEqual(digestStream.bytesWritten, 5n);
  },
};

export const pipeThroughIntoDigest = {
  async test() {
    const enc = new TextEncoder();
    const digestStream = new crypto.DigestStream('crc32');
    const { readable, writable } = new TransformStream();
    const writer = writable.getWriter();
    const pipe = readable.pipeTo(digestStream);
    await writer.write(enc.encode('Hello world'));
    await writer.close();
    await pipe;
    // Same expectation as the shared AWS CRC vector for 'Hello world'.
    deepStrictEqual(
      new Uint8Array(await digestStream.digest),
      new Uint8Array([0x8b, 0xd6, 0x9e, 0x52])
    );
  },
};

export const responseBodyIntoDigest = {
  async test() {
    const digestStream = new crypto.DigestStream('md5');
    await new Response('hello').body.pipeTo(digestStream);
    deepStrictEqual(new Uint8Array(await digestStream.digest), helloMd5);
    strictEqual(digestStream.bytesWritten, 5n);
  },
};
