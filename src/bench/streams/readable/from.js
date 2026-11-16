// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream.from() over an array and over sync and async generators,
// read to the end.

import { CHUNK, drainReader, expectEqual } from 'helpers';

function* syncChunks(count) {
  for (let i = 0; i < count; i++) yield CHUNK;
}

async function* asyncChunks(count) {
  for (let i = 0; i < count; i++) yield CHUNK;
}

async function drain(stream, count) {
  expectEqual(await drainReader(stream.getReader()), count, 'chunks read');
}

export const from = {
  bench(b) {
    for (const count of [16, 256]) {
      const array = new Array(count).fill(CHUNK);
      b.run(`array x${count}`, () => drain(ReadableStream.from(array), count));
    }
    b.run('sync generator x256', () =>
      drain(ReadableStream.from(syncChunks(256)), 256)
    );
    b.run('async generator x256', () =>
      drain(ReadableStream.from(asyncChunks(256)), 256)
    );
  },
};
