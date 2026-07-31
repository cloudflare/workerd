// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { rejects } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-132.
// ReaderImpl::read() holds only a raw ReadableStreamController& across the
// synchronous pull() callback. If pull() calls reader.releaseLock(), the
// jsg::Ref<ReadableStream> in the Attached state is destroyed. gc() then
// frees the stream/controller/ValueReadable while the C++ stack is inside
// read(). The fix holds a local addRef() in ReaderImpl::read().
export const releaseLockInsidePullDefaultReader = {
  async test() {
    let reader;
    reader = new ReadableStream(
      {
        pull(_c) {
          reader.releaseLock();
          gc();
        },
      },
      { highWaterMark: 0 }
    ).getReader();
    gc();

    await rejects(reader.read(), {
      message: /This ReadableStream reader has been released/,
    });
  },
};

// Same test but with a BYOB reader to cover the ByteReadable path.
export const releaseLockInsidePullByobReader = {
  async test() {
    let reader;
    reader = new ReadableStream({
      type: 'bytes',
      pull(_c) {
        reader.releaseLock();
        gc();
      },
    }).getReader({ mode: 'byob' });
    gc();

    await rejects(reader.read(new Uint8Array(16)), {
      message: /This ReadableStream reader has been released/,
    });
  },
};
