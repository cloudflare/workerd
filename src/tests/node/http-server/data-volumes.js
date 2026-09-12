// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Volumes and shapes through the server: a large request body across the
// binding, thousands of tiny response writes, alternating write shapes,
// and a multi-byte UTF-8 sequence split across body chunks.

import { Buffer } from 'node:buffer';
import { strictEqual, ok } from 'node:assert';
import { withServer, remember, dispatch, manualStream } from 'harness';

const MODULUS = 251;

// An 8 MiB body pumped across the service binding arrives whole and in
// order, in more than one chunk.
export const largeRequestBodyThroughBinding = {
  async test(ctrl, env) {
    const SIZE = 8 * 1024 * 1024;
    const body = new Uint8Array(SIZE);
    for (let i = 0; i < SIZE; i++) body[i] = i % MODULUS;
    await withServer(
      (req, res) => {
        let received = 0;
        let chunks = 0;
        let broken = -1;
        req.on('data', (chunk) => {
          chunks++;
          for (let i = 0; i < chunk.length && broken < 0; i++) {
            if (chunk[i] !== (received + i) % MODULUS) broken = received + i;
          }
          received += chunk.length;
        });
        req.on('end', () =>
          res.end(JSON.stringify({ received, chunks, broken }))
        );
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body,
        });
        const { received, chunks, broken } = await res.json();
        strictEqual(received, SIZE);
        strictEqual(broken, -1);
        ok(chunks > 1, `${chunks} chunks`);
      }
    );
  },
};

// Twenty thousand one-byte res.write()s: every byte reaches the client, in
// order.
export const manyTinyResponseWrites = {
  async test(ctrl, env) {
    const TOTAL = 20_000;
    await withServer(
      (req, res) => {
        res.writeHead(200);
        for (let i = 0; i < TOTAL; i++) res.write(Buffer.from([i % MODULUS]));
        res.end();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        const bytes = new Uint8Array(await res.arrayBuffer());
        strictEqual(bytes.length, TOTAL);
        for (let i = 0; i < TOTAL; i++) {
          if (bytes[i] !== i % MODULUS)
            strictEqual(bytes[i], i % MODULUS, `byte ${i}`);
        }
      }
    );
  },
};

// Strings, Buffers, Uint8Arrays and empty writes alternating: the body is
// their concatenation.
export const alternatingWriteShapes = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(200);
        let expected = '';
        for (let i = 0; i < 300; i++) {
          const piece = `p${i}|`;
          expected += piece;
          switch (i % 4) {
            case 0:
              res.write(piece);
              break;
            case 1:
              res.write(Buffer.from(piece));
              break;
            case 2:
              res.write(new TextEncoder().encode(piece));
              break;
            default:
              res.write('');
              res.write(piece, 'utf8');
          }
        }
        res.end(`|${expected.length}`);
      },
      async () => {
        const text = await (await env.SERVICE.fetch('http://x/')).text();
        const expected = Array.from({ length: 300 }, (_, i) => `p${i}|`).join(
          ''
        );
        strictEqual(text, `${expected}|${expected.length}`);
      }
    );
  },
};

// A body whose multi-byte UTF-8 sequences are split across chunks, read
// with setEncoding('utf8'): reassembled exactly, no replacement character.
export const splitUtf8ReassembledBySetEncoding = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const text = 'héllo wörld — 日本語 🎉 fin';
    const bytes = new TextEncoder().encode(text);
    const { stream, controller } = manualStream();
    await withServer(
      (req, res) => {
        req.setEncoding('utf8');
        let received = '';
        req.on('data', (chunk) => (received += chunk));
        req.on('end', () => res.end(received));
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        for (let i = 0; i < bytes.length; i++) {
          controller.enqueue(bytes.slice(i, i + 1));
          if (i % 7 === 0) await scheduler.wait(1);
        }
        controller.close();
        const echoed = await (await pending).text();
        strictEqual(echoed, text);
        strictEqual(echoed.includes('\uFFFD'), false);
      }
    );
  },
};
