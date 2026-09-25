// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Symbol.dispose errors the digest without touching the WritableStream
// state: the stream stays unlocked and writable-looking, the error surfaces
// on the digest promise and on the first non-empty write.

import { strictEqual, rejects } from 'node:assert';

const disposedMsg = 'The DigestStream was disposed.';

export const disposeErrorsDigestAndWrites = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();
    strictEqual(stream.locked, false);
    const writer = stream.getWriter();
    await rejects(writer.write(new TextEncoder().encode('hello')), {
      message: disposedMsg,
    });
    await rejects(stream.digest, { message: disposedMsg });
  },
};

export const disposeThenZeroLengthWrite = {
  async test() {
    // The sink short-circuits on length before it looks at the digest
    // state, so zero-length writes still resolve after dispose; non-empty
    // writes reject.
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();
    const writer = stream.getWriter();
    await writer.write(new Uint8Array(0));
    await writer.write('');
    await rejects(writer.write(new Uint8Array(1)), { message: disposedMsg });
    await rejects(stream.digest, { message: disposedMsg });
  },
};

export const disposeIsIdempotent = {
  async test() {
    const stream = new crypto.DigestStream('md5');
    stream[Symbol.dispose]();
    stream[Symbol.dispose]();
    stream[Symbol.dispose]();
    await rejects(stream.digest, { message: disposedMsg });
  },
};

export const disposeAfterCloseIsNoOp = {
  async test() {
    // Closing resolves the digest; a later dispose must not overwrite it.
    const stream = new crypto.DigestStream('md5');
    const writer = stream.getWriter();
    await writer.write('hello');
    await writer.close();
    const digest = await stream.digest;
    stream[Symbol.dispose]();
    strictEqual(await stream.digest, digest);
  },
};
