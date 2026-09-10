// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Stream lifecycle: close resolves the digest, abort rejects it, and
// abandonment in any state neither crashes nor settles it.

import { strictEqual, rejects } from 'node:assert';

export const closeResolvesDigest = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    strictEqual((await stream.digest).byteLength, 16);
  },
};

export const abortRejectsDigest = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const reason = new Error('boom');
    await writer.abort(reason);
    await rejects(stream.digest, { message: 'boom' });
  },
};

export const abortAfterWritesRejectsDigest = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();
    const writesSettled = Promise.allSettled([
      writer.write(enc.encode('hello')),
      writer.write(enc.encode('there')),
    ]);
    await writer.abort(new Error('boom'));
    await writesSettled;
    await rejects(stream.digest);
  },
};

export const writeAfterCloseRejects = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.close();
    await rejects(writer.write(new Uint8Array(1)));
    // The digest is unaffected by the failed write.
    strictEqual((await stream.digest).byteLength, 16);
    strictEqual(stream.bytesWritten, 0n);
  },
};

export const doubleCloseIsSafe = {
  async test() {
    // The digest is single-use: a second close must not try to finalize
    // the native context again.
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.close();
    await rejects(writer.close());
    strictEqual((await stream.digest).byteLength, 16);
  },
};

export const abandonedStreamDoesNotCrash = {
  async test() {
    // Never closed or aborted: the digest simply never settles.
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    const enc = new TextEncoder();
    writer.write(enc.encode('hello'));
    writer.write(enc.encode('there'));
  },
};

export const abandonedWriterDoesNotThrow = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write('hello');
    writer.releaseLock();
    strictEqual(stream.bytesWritten, 5n);
    strictEqual(stream.locked, false);
  },
};

export const unusedStreamDoesNotCrash = {
  test() {
    new crypto.DigestStream('SHA-1');
  },
};
