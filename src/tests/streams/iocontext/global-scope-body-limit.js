// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The limit on the bytes a body read buffers. Outside any IoContext, as at
// GLOBAL SCOPE, there is no limit enforcer to ask, and a body is read within a
// fixed cap: 32 MiB, or 1 MiB in predictable mode, which wd_test() runs workerd
// in. Inside a request, the enforcer's limit applies; workerd's sets none.

import { deepStrictEqual, match, ok } from 'node:assert';

const kLimit = 1024 * 1024;

// A stream body of `size` bytes in 64 KiB chunks, which declares no length, so
// the read finds out how long it is only by reading it.
function streamOf(size) {
  let left = size;
  return new ReadableStream({
    pull(c) {
      const n = Math.min(left, 64 * 1024);
      if (n === 0) {
        c.close();
        return;
      }
      c.enqueue(new Uint8Array(n));
      left -= n;
    },
  });
}

// Reads `body` with `method`, settled, so that a read that fails at global
// scope reaches the test rather than going unhandled.
const read = (method, body) =>
  new Response(body)[method]().then(
    (result) => ({
      length: method === 'text' ? result.length : result.byteLength,
    }),
    (error) => ({ error })
  );

function assertOverLimit({ error }, what) {
  ok(error instanceof TypeError, `${what}: ${error}`);
  match(error.message, /^Memory limit/, what);
}

const kMethods = ['arrayBuffer', 'text'];

const globalScopeReads = kMethods.map((method) => ({
  method,
  atLimit: read(method, new Uint8Array(kLimit)),
  sourceOverLimit: read(method, new Uint8Array(kLimit + 1)),
  streamAtLimit: read(method, streamOf(kLimit)),
  streamOverLimit: read(method, streamOf(kLimit + 1)),
}));

export const globalScopeBodyLimit = {
  async test() {
    for (const { method, ...reads } of globalScopeReads) {
      // The method is part of the value compared, so that a failure shows it
      // beside what the read produced.
      deepStrictEqual(
        { method, ...(await reads.atLimit) },
        { method, length: kLimit }
      );
      assertOverLimit(await reads.sourceOverLimit, method);
      deepStrictEqual(
        { method, ...(await reads.streamAtLimit) },
        { method, length: kLimit }
      );
      assertOverLimit(await reads.streamOverLimit, `${method} of a stream`);
    }
  },
};

export const requestBodyLimit = {
  async test() {
    for (const method of kMethods) {
      deepStrictEqual(
        { method, ...(await read(method, new Uint8Array(kLimit + 1))) },
        { method, length: kLimit + 1 }
      );
      deepStrictEqual(
        { method, stream: true, ...(await read(method, streamOf(kLimit + 1))) },
        { method, stream: true, length: kLimit + 1 }
      );
    }
  },
};
