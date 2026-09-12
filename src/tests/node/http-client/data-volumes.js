// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Volumes through the client: a large response read with pauses, many
// chunked pieces, a body made of thousands of tiny writes, a large body.

import { Buffer } from 'node:buffer';
import { strictEqual } from 'node:assert';
import { request, get, response, collect } from 'harness';

const MODULUS = 251;

// An 8 MiB response (the sidecar's byte pattern) consumed with a pause()
// after every megabyte: every byte, in order.
export const largeResponseWithPauses = {
  async test(ctrl, env) {
    const SIZE = 8 * 1024 * 1024;
    const res = await response(get(env, `/large?bytes=${SIZE}`));
    let received = 0;
    let sinceLastPause = 0;
    let pauses = 0;
    await new Promise((resolve, reject) => {
      res.on('data', (chunk) => {
        for (let i = 0; i < chunk.length; i++) {
          if (chunk[i] !== (received + i) % MODULUS) {
            reject(new Error(`pattern break at byte ${received + i}`));
            return;
          }
        }
        received += chunk.length;
        sinceLastPause += chunk.length;
        if (sinceLastPause >= 1024 * 1024) {
          sinceLastPause = 0;
          pauses++;
          res.pause();
          setTimeout(() => res.resume(), 2);
        }
      });
      res.on('end', resolve);
      res.on('error', reject);
    });
    strictEqual(received, SIZE);
    strictEqual(res.complete, true);
    strictEqual(pauses >= 7, true, `${pauses} pauses`);
  },
};

// Five hundred chunked pieces written back to back: the concatenation is
// exact (the pieces may arrive coalesced).
export const manyChunkedPieces = {
  async test(ctrl, env) {
    const res = await response(get(env, '/chunked?n=500&delay=0'));
    strictEqual(
      (await collect(res)).toString(),
      Array.from({ length: 500 }, (_, i) => `chunk-${i}|`).join('')
    );
  },
};

// Ten thousand one-byte write()s make one body of ten thousand bytes, in
// order.
export const tenThousandTinyWritesFormOneBody = {
  async test(ctrl, env) {
    const TOTAL = 10_000;
    const req = request(env, '/echo', { method: 'POST' });
    for (let i = 0; i < TOTAL; i++) req.write(Buffer.from([i % MODULUS]));
    req.end();
    const res = await response(req);
    strictEqual(res.headers['x-request-content-length'], String(TOTAL));
    const body = await collect(res);
    strictEqual(body.length, TOTAL);
    for (let i = 0; i < TOTAL; i++) {
      if (body[i] !== i % MODULUS)
        strictEqual(body[i], i % MODULUS, `byte ${i}`);
    }
  },
};

// An 8 MiB request body arrives whole at the server.
export const largeRequestBody = {
  async test(ctrl, env) {
    const SIZE = 8 * 1024 * 1024;
    const body = Buffer.alloc(SIZE);
    for (let i = 0; i < SIZE; i++) body[i] = i % MODULUS;
    const req = request(env, '/sink', { method: 'POST' });
    req.end(body);
    const res = await response(req);
    const summary = JSON.parse((await collect(res)).toString());
    strictEqual(summary.bytes, SIZE);
    strictEqual(summary.contentLength, String(SIZE));
  },
};
