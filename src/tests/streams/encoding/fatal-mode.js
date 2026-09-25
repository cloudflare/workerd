// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// TextDecoderStream fatal mode: a decode failure rejects the write and
// errors both sides with the same TypeError.

import { strictEqual, rejects } from 'node:assert';

const check = (err) => {
  strictEqual(err.constructor, TypeError);
  strictEqual(err.message, 'Failed to decode input.');
  return true;
};

export const fatalInvalidBytesErrorStream = {
  async test() {
    const tds = new TextDecoderStream('utf-8', { fatal: true });
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const readPromise = reader.read();
    await rejects(writer.write(Uint8Array.of(0xff)), check);
    await rejects(readPromise, check);
    await rejects(writer.closed, check);
    await rejects(reader.closed, check);
  },
};

export const fatalIncompleteAtCloseRejects = {
  async test() {
    // An incomplete sequence is fine while streaming, but close() runs the
    // final flush decode, which throws in fatal mode.
    const tds = new TextDecoderStream('utf-8', { fatal: true });
    const writer = tds.writable.getWriter();
    const reader = tds.readable.getReader();
    const readPromise = reader.read();
    await writer.write(Uint8Array.of(0xe4, 0xb8));
    await rejects(writer.close(), check);
    await rejects(readPromise, check);
  },
};
