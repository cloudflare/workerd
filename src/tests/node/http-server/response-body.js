// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The response body: the ServerResponse hands a `new ReadableStream({
// type: 'bytes' })` to the Response it resolves when headers are sent
// (writes before that are buffered and flushed into it), enqueues later
// writes as they come, trims to a declared Content-Length, and closes the
// stream on 'finish'. The fetch() therefore resolves as soon as the headers
// go out and the body streams after.

import { pipeline } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, ok, deepStrictEqual, throws } from 'node:assert';
import { withServer } from 'harness';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A first write() sends the headers implicitly; every chunk type arrives in
// order, empty writes contribute nothing.
export const implicitHeadersAndChunkTypes = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.setHeader('X-Custom', 'mixed');
        res.write('string|');
        res.write(Buffer.from('buffer|'));
        res.write(new Uint8Array([85, 56, 124])); // "U8|"
        res.write('');
        res.write('utf8-ñ|', 'utf8');
        setTimeout(() => res.end('async'), 10);
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 200);
        strictEqual(res.headers.get('X-Custom'), 'mixed');
        strictEqual(await res.text(), 'string|buffer|U8|utf8-ñ|async');
      }
    );
  },
};

// The Response resolves once the headers go out (writeHead() only formats
// them; the first write sends them), while the handler is still writing;
// chunks written afterwards are readable as they are written, before end().
export const bodyStreamsBeforeEnd = {
  async test(ctrl, env) {
    let serverRes;
    await withServer(
      (req, res) => {
        serverRes = res;
        res.writeHead(200);
        res.write('first');
        setTimeout(() => res.write('second'), 40);
        setTimeout(() => res.end('last'), 80);
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(serverRes.writableEnded, false);
        const reader = res.body.getReader();
        strictEqual(dec.decode((await reader.read()).value), 'first');
        strictEqual(serverRes.writableEnded, false);
        strictEqual(dec.decode((await reader.read()).value), 'second');
        let rest = '';
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          rest += dec.decode(value);
        }
        strictEqual(rest, 'last');
        strictEqual(serverRes.writableFinished, true);
      }
    );
  },
};

// Many small writes and a 640 KiB body arrive whole.
export const largeAndManyWrites = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        if (req.url === '/large') {
          res.writeHead(200);
          for (let i = 0; i < 10; i++) res.write(Buffer.alloc(64 * 1024, i));
          res.end();
        } else {
          res.writeHead(200);
          for (let i = 0; i < 100; i++) res.write(`${i}|`);
          res.end('END');
        }
      },
      async () => {
        const large = await env.SERVICE.fetch('http://x/large');
        const bytes = new Uint8Array(await large.arrayBuffer());
        strictEqual(bytes.byteLength, 64 * 1024 * 10);
        for (let i = 0; i < 10; i++) strictEqual(bytes[i * 64 * 1024], i);
        const many = await env.SERVICE.fetch('http://x/many');
        const text = await many.text();
        ok(text.startsWith('0|1|2|'));
        ok(text.endsWith('98|99|END'));
      }
    );
  },
};

// A declared Content-Length caps the body: extra bytes are dropped, fewer
// bytes are sent as they are.
export const contentLengthCapsBody = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        if (req.url === '/too-few') {
          res.writeHead(200, { 'Content-Length': '20' });
          res.write('0123456789');
          res.end();
        } else if (req.url === '/too-many') {
          res.writeHead(200, { 'Content-Length': '10' });
          res.write('0123456789');
          res.write('0123456789');
          res.end();
        } else {
          res.writeHead(200, { 'Content-Length': '15' });
          res.write('Hello ');
          res.write('World!!!');
          res.end('!');
        }
      },
      async () => {
        strictEqual(
          await (await env.SERVICE.fetch('http://x/too-few')).text(),
          '0123456789'
        );
        strictEqual(
          await (await env.SERVICE.fetch('http://x/too-many')).text(),
          '0123456789'
        );
        strictEqual(
          await (await env.SERVICE.fetch('http://x/exact')).text(),
          'Hello World!!!!'
        );
      }
    );
  },
};

// 204 and 304 responses have no body: writes are dropped and the Response
// body is null.
export const noBodyStatuses = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(Number(req.url.slice(1)));
        res.write('ignored');
        res.end('also ignored');
      },
      async () => {
        for (const status of [204, 304]) {
          const res = await env.SERVICE.fetch(`http://x/${status}`);
          strictEqual(res.status, status);
          strictEqual(res.body, null);
          strictEqual(await res.text(), '');
        }
      }
    );
  },
};

// The reply to a HEAD has no body: the response is marked bodiless before
// the handler runs, its writes are accepted (callback and all) and dropped,
// and the client sees the headers with a null body.
export const headResponseHasNoBody = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        events.push(`hasBody:${res._hasBody}`);
        res.writeHead(200, { 'Content-Length': '5' });
        const accepted = res.write('hel', (err) =>
          events.push(`writecb:${err?.code ?? 'ok'}`)
        );
        events.push(`write:${accepted}`);
        res.end('lo', () => events.push('finish'));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', { method: 'HEAD' });
        strictEqual(res.status, 200);
        strictEqual(res.headers.get('Content-Length'), '5');
        strictEqual(res.body, null);
        await scheduler.wait(5);
        deepStrictEqual(events, [
          'hasBody:false',
          'write:true',
          'writecb:ok',
          'finish',
        ]);
      }
    );
  },
};

// With the server's rejectNonStandardBodyWrites option, a body write on a
// bodiless response (204, or the reply to a HEAD) throws
// ERR_HTTP_BODY_NOT_ALLOWED instead of being dropped; the response still
// completes without one.
export const rejectNonStandardBodyWritesThrows = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(req.method === 'HEAD' ? 200 : 204);
        throws(() => res.write('body'), { code: 'ERR_HTTP_BODY_NOT_ALLOWED' });
        throws(() => res.end('body'), { code: 'ERR_HTTP_BODY_NOT_ALLOWED' });
        res.end();
      },
      async () => {
        const noContent = await env.SERVICE.fetch('http://x/');
        strictEqual(noContent.status, 204);
        strictEqual(noContent.body, null);
        const head = await env.SERVICE.fetch('http://x/', { method: 'HEAD' });
        strictEqual(head.status, 200);
        strictEqual(head.body, null);
      },
      { rejectNonStandardBodyWrites: true }
    );
  },
};

// cork()/uncork(): corked writes queue (writableLength counts the header
// bytes too) and flush together.
export const corkAndUncork = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(200, { 'Content-Type': 'text/plain' });
        strictEqual(res.writableLength, 0);
        res.cork();
        strictEqual(res.writableLength, 0);
        res.write('chunk1');
        strictEqual(res.writableLength, 108);
        res.write('chunk2');
        strictEqual(res.writableLength, 114);
        res.write('chunk3');
        strictEqual(res.writableLength, 120);
        res.uncork();
        strictEqual(res.writableLength, 0);
        res.end('final');
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'chunk1chunk2chunk3final');
      }
    );
  },
};

// write() reports backpressure against the response's own buffer, 'drain'
// follows, and every chunk still arrives. (The client's consumption does
// not feed back: the body stream queues what the handler writes.)
export const backpressureSignaling = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.writeHead(200, { 'Content-Type': 'application/octet-stream' });
        let writes = 0;
        const continueWriting = () => {
          while (writes < 50) {
            const accepted = res.write(Buffer.alloc(32 * 1024, writes % 256));
            events.push({
              type: 'write',
              accepted,
              writableLength: res.writableLength,
            });
            writes++;
            if (!accepted) {
              events.push({ type: 'backpressure' });
              return;
            }
          }
          res.end();
        };
        res.on('drain', () => {
          events.push({ type: 'drain' });
          continueWriting();
        });
        continueWriting();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual((await res.arrayBuffer()).byteLength, 50 * 32 * 1024);
        const writes = events.filter((e) => e.type === 'write');
        strictEqual(writes.length, 50);
        for (const e of writes) {
          strictEqual(typeof e.accepted, 'boolean');
          ok(e.writableLength >= 0);
        }
        const backpressure = events.filter(
          (e) => e.type === 'backpressure'
        ).length;
        const drains = events.filter((e) => e.type === 'drain').length;
        strictEqual(drains, backpressure);
      }
    );
  },
};

// The server's highWaterMark option is the response's writableHighWaterMark,
// but once the headers are out every write() reports acceptance: the body
// stream queues whatever the handler writes, so no 'drain' is ever owed.
export const writesAlwaysAcceptedAfterHeaders = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        strictEqual(res.writableHighWaterMark, 16);
        res.writeHead(200);
        const accepted = res.write(Buffer.alloc(32, 0x61));
        strictEqual(res.writableNeedDrain, false);
        res.end(String(accepted));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), `${'a'.repeat(32)}true`);
      },
      { highWaterMark: 16 }
    );
  },
};

// A web ReadableStream pumped into the response through pipeline().
export const webSourcePipelinedIntoResponse = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(200);
        const source = new ReadableStream({
          start(controller) {
            controller.enqueue(enc.encode('from '));
            controller.enqueue(enc.encode('a web source'));
            controller.close();
          },
        });
        pipeline(source, res, (err) => {
          if (err) res.destroy(err);
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'from a web source');
      }
    );
  },
};

// 'finish' fires once the body has been handed off; 'close' follows it.
export const finishThenClose = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('finish', () => events.push(`finish:${res.closed}`));
        res.on('close', () => events.push(`close:${res.closed}`));
        res.end('bye');
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'bye');
        await scheduler.wait(5);
        deepStrictEqual(events, ['finish:false', 'close:true']);
      }
    );
  },
};

// Writes after end() fail with ERR_STREAM_WRITE_AFTER_END (and a second
// end() is inert); the body already sent is unaffected.
export const writeAfterEndFails = {
  async test(ctrl, env) {
    const errors = [];
    await withServer(
      (req, res) => {
        res.write('hello');
        res.end();
        queueMicrotask(() => {
          res.end('world');
          res.write('world', (err) => errors.push(err.code));
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'hello');
        await scheduler.wait(5);
        deepStrictEqual(errors, ['ERR_STREAM_WRITE_AFTER_END']);
      }
    );
  },
};

// A Content-Length the handler sets is taken at parseInt's word, and is
// what the client receives: a non-numeric one leaves the body uncapped, a
// zero or negative one drops every chunk, a fraction or padded number caps
// at its integer part.
export const contentLengthLies = {
  async test(ctrl, env) {
    const cases = {
      '/abc': ['abc', 'abc', '0123456789'],
      '/zero': ['0', '0', ''],
      '/negative': ['-5', '-5', ''],
      '/fraction': ['5.9', '5.9', '01234'],
      '/padded': [' 4 ', '4', '0123'],
    };
    await withServer(
      (req, res) => {
        res.writeHead(200, { 'Content-Length': cases[req.url][0] });
        res.write('0123456789');
        res.end();
      },
      async () => {
        for (const [path, [, header, body]] of Object.entries(cases)) {
          const res = await env.SERVICE.fetch(`http://x${path}`);
          strictEqual(res.status, 200);
          strictEqual(res.headers.get('content-length'), header);
          strictEqual(await res.text(), body);
        }
      }
    );
  },
};
