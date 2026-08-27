// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Close/cancel/abort propagation through the transform. These are standard
// (JS-controller) streams in both implementations, so the reason crossing
// the transform is the ORIGINAL instance — unlike the identity streams'
// kj-boundary re-creation.

import { strictEqual, rejects } from 'node:assert';

export const closeResolvesPendingRead = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reader = tes.readable.getReader();
    const readPromise = reader.read();
    await writer.close();
    strictEqual((await readPromise).done, true);
    await reader.closed;
    await writer.closed;
  },
};

export const cancelReasonReachesWriter = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const reason = new RangeError('boom');
    const closedPromise = rejects(writer.closed, (err) => err === reason);
    await tes.readable.cancel(reason);
    await closedPromise;
    // Later writes reject with the same instance.
    await rejects(writer.write('x'), (err) => err === reason);
  },
};

export const abortReasonReachesReader = {
  async test() {
    const tds = new TextDecoderStream();
    const reader = tds.readable.getReader();
    const reason = new RangeError('bam');
    const readPromise = reader.read();
    await tds.writable.abort(reason);
    await rejects(readPromise, (err) => err === reason);
    await rejects(reader.closed, (err) => err === reason);
  },
};
