// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The request body: the Request's ReadableStream pumped into the
// IncomingMessage by a default reader, one read() per _read(), with the
// Readable's push() return value as backpressure. A GET has no body stream
// and ends at once.

import { Writable, pipeline } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { withServer, manualStream } from 'harness';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A GET request: no 'data', 'end' at once, complete.
export const getRequestEndsImmediately = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        const events = [];
        req.on('data', () => events.push('data'));
        req.on('end', () => {
          events.push('end');
          res.end(
            JSON.stringify({
              events,
              complete: req.complete,
              ended: req.readableEnded,
              contentLength: req.headers['content-length'] ?? null,
            })
          );
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        deepStrictEqual(await res.json(), {
          events: ['end'],
          complete: true,
          ended: true,
          contentLength: null,
        });
      }
    );
  },
};

// A POST body arrives as Buffer chunks (strings with setEncoding), then
// 'end' with complete set.
export const postBodyArrivesAsBuffers = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        if (req.url === '/text') req.setEncoding('utf8');
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
          res.end(
            JSON.stringify({
              types: chunks.map((c) =>
                Buffer.isBuffer(c) ? 'Buffer' : typeof c
              ),
              text:
                req.url === '/text'
                  ? chunks.join('')
                  : Buffer.concat(chunks).toString(),
              complete: req.complete,
            })
          );
        });
      },
      async () => {
        const raw = await env.SERVICE.fetch('http://x/raw', {
          method: 'POST',
          body: 'hello world',
        });
        const rawBody = await raw.json();
        deepStrictEqual(rawBody.types, ['Buffer']);
        strictEqual(rawBody.text, 'hello world');
        strictEqual(rawBody.complete, true);
        const text = await env.SERVICE.fetch('http://x/text', {
          method: 'POST',
          body: 'héllo wörld',
        });
        const textBody = await text.json();
        deepStrictEqual(textBody.types, ['string']);
        strictEqual(textBody.text, 'héllo wörld');
      }
    );
  },
};

// A 'data' listener attached inside the handler, after the body may already
// have been buffered, still receives the data; a GET afterwards emits only
// 'end'.
export const lateDataListenerReceivesBody = {
  async test(ctrl, env) {
    const seen = [];
    await withServer(
      (req, res) => {
        const events = [];
        req.on('data', (chunk) => {
          ok(Buffer.isBuffer(chunk));
          events.push(`data:${chunk}`);
        });
        req.on('error', (err) => events.push(`error:${err.message}`));
        req.on('end', () => {
          seen.push(events);
          res.end('OK');
        });
      },
      async () => {
        await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: 'test data',
        });
        await env.SERVICE.fetch('http://x/');
        deepStrictEqual(seen, [['data:test data'], []]);
      }
    );
  },
};

// A 256 KiB body streams through in several 'data' events, bytes intact.
export const largeBodyArrivesInChunks = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        let events = 0;
        let total = 0;
        let intact = true;
        req.on('data', (chunk) => {
          events++;
          total += chunk.length;
          for (const byte of chunk) if (byte !== 123) intact = false;
        });
        req.on('end', () => {
          res.end(JSON.stringify({ events, total, intact }));
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: Buffer.alloc(256 * 1024, 123),
        });
        const result = await res.json();
        ok(
          result.events > 1,
          `expected several data events, got ${result.events}`
        );
        strictEqual(result.total, 256 * 1024);
        strictEqual(result.intact, true);
      }
    );
  },
};

// A streaming request body (a ReadableStream the client feeds by hand)
// reaches the handler chunk by chunk: the server echoes each chunk into the
// response as it arrives and the client reads it before sending the next.
// Such a body is chunked, with no Content-Length.
export const streamingBodyArrivesIncrementally = {
  async test(ctrl, env) {
    const { stream, controller } = manualStream();
    await withServer(
      (req, res) => {
        res.writeHead(200, {
          'x-request-te': req.headers['transfer-encoding'] ?? 'none',
          'x-request-cl': req.headers['content-length'] ?? 'none',
        });
        req.on('data', (chunk) => res.write(chunk));
        req.on('end', () => res.end());
      },
      async () => {
        const pending = env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: stream,
        });
        controller.enqueue(enc.encode('one'));
        const res = await pending;
        strictEqual(res.headers.get('x-request-te'), 'chunked');
        strictEqual(res.headers.get('x-request-cl'), 'none');
        const reader = res.body.getReader();
        strictEqual(dec.decode((await reader.read()).value), 'one');
        controller.enqueue(enc.encode('two'));
        strictEqual(dec.decode((await reader.read()).value), 'two');
        controller.close();
        strictEqual((await reader.read()).done, true);
      }
    );
  },
};

// A FixedLengthStream body announces its length: the handler sees a
// Content-Length header and the full body.
export const fixedLengthBodyCarriesContentLength = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
          res.end(
            JSON.stringify({
              contentLength: req.headers['content-length'],
              body: Buffer.concat(chunks).toString(),
            })
          );
        });
      },
      async () => {
        const fixed = new FixedLengthStream(5);
        const writer = fixed.writable.getWriter();
        // The identity stream settles writes on consumption: do not await
        // them ahead of the fetch that consumes the readable side.
        const written = Promise.all([
          writer.write(enc.encode('12345')),
          writer.close(),
        ]);
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: fixed.readable,
        });
        await written;
        deepStrictEqual(await res.json(), {
          contentLength: '5',
          body: '12345',
        });
      }
    );
  },
};

// pause() holds the body (no 'data' while paused, nothing lost), resume()
// continues it.
export const pausedBodyResumesWithoutLoss = {
  async test(ctrl, env) {
    const { stream, controller } = manualStream();
    await withServer(
      (req, res) => {
        const events = [];
        req.on('data', (chunk) => {
          events.push(`data:${chunk}`);
          if (events.length === 1) {
            req.pause();
            events.push('pause');
            setTimeout(() => {
              events.push('resume');
              req.resume();
            }, 60);
          }
        });
        req.on('end', () => res.end(JSON.stringify(events)));
      },
      async () => {
        const pending = env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: stream,
        });
        for (let i = 0; i < 4; i++) {
          controller.enqueue(enc.encode(`c${i}`));
          await scheduler.wait(10);
        }
        controller.close();
        const events = await (await pending).json();
        deepStrictEqual(events, [
          'data:c0',
          'pause',
          'resume',
          'data:c1',
          'data:c2',
          'data:c3',
        ]);
      }
    );
  },
};

// req.pipe(res): the request body echoed straight back.
export const echoThroughPipe = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(200);
        req.pipe(res);
      },
      async () => {
        const data = Buffer.alloc(128 * 1024, 42);
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: data,
        });
        ok(Buffer.from(await res.arrayBuffer()).equals(data));
      }
    );
  },
};

// The request piped to several node destinations at once: each receives
// the whole body.
export const bodyPipedToSeveralDestinations = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        const received = [[], [], []];
        let finished = 0;
        for (const chunks of received) {
          const dest = new Writable({
            write(chunk, encoding, callback) {
              chunks.push(chunk);
              callback();
            },
          });
          dest.on('finish', () => {
            if (++finished === 3) {
              res.end(
                JSON.stringify(received.map((c) => Buffer.concat(c).toString()))
              );
            }
          });
          req.pipe(dest);
        }
      },
      async () => {
        const text =
          'Hello from multiple pipes! This should reach all destinations.';
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: text,
        });
        deepStrictEqual(await res.json(), [text, text, text]);
      }
    );
  },
};

// pipeline(req, TransformStream, res): the body through a web transform
// into the response.
export const bodyThroughWebTransformPipeline = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.writeHead(200);
        const upper = new TransformStream({
          transform(chunk, controller) {
            controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
          },
        });
        pipeline(req, upper, res, (err) => {
          if (err) res.destroy(err);
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: 'shout this',
        });
        strictEqual(await res.text(), 'SHOUT THIS');
      }
    );
  },
};
