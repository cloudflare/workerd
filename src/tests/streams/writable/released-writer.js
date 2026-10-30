// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A writer after releaseLock(): every member but releaseLock() fails with
// the released-writer TypeError, whose text differs by implementation
// (ledger #12).

import { strictEqual, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const releasedWriterError = {
  name: 'TypeError',
  message: usingTsImpl
    ? 'This writer has been released'
    : 'This WritableStream writer has been released.',
};

// write(), abort() and close() reject, desiredSize throws, and ready and
// closed reject with the error the release made; a second releaseLock()
// does nothing.
export const releasedWriterMembers = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    writer.releaseLock();
    await rejects(writer.write('a'), releasedWriterError);
    await rejects(writer.abort(), releasedWriterError);
    await rejects(writer.close(), releasedWriterError);
    throws(() => writer.desiredSize, releasedWriterError);
    await rejects(writer.ready, releasedWriterError);
    await rejects(writer.closed, releasedWriterError);
    strictEqual(writer.releaseLock(), undefined);
    strictEqual(ws.locked, false);
  },
};
