// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// req.pipe(destination) is the Readable's pipe(): the destination's
// backpressure pauses the request body and 'drain' resumes it, the
// destination announces 'pipe' and 'unpipe', a destination that errors is
// unpiped (and nothing more is written to it), unpipe() stops delivery
// and pauses a source left without destinations, and a source error is
// not the destination's business — that is pipeline()'s job.

import { Writable } from 'node:stream';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { withServer, dispatch, remember, manualStream } from 'harness';

const enc = new TextEncoder();

// A sink that completes each write on a timer, with a small buffer.
function slowSink(highWaterMark, delay = 1) {
  const sink = new Writable({
    highWaterMark,
    write(chunk, encoding, callback) {
      sink.bytes += chunk.length;
      sink.largestChunk = Math.max(sink.largestChunk, chunk.length);
      sink.maxBuffered = Math.max(sink.maxBuffered, sink.writableLength);
      setTimeout(callback, delay);
    },
  });
  sink.bytes = 0;
  sink.largestChunk = 0;
  sink.maxBuffered = 0;
  return sink;
}

// A recording sink: what it received, and the events it saw.
function recordingSink(events, req) {
  const received = [];
  const dest = new Writable({
    write(chunk, encoding, callback) {
      received.push(chunk.toString());
      callback();
    },
  });
  dest.received = received;
  dest.on('pipe', (src) => events.push(`pipe(${src === req})`));
  dest.on('unpipe', (src) => events.push(`unpipe(${src === req})`));
  dest.on('error', (err) => events.push(`error(${err.message})`));
  dest.on('finish', () => events.push('finish'));
  return dest;
}

// A megabyte piped into a slow sink with a 16 KiB buffer: the sink never
// holds more than its high-water mark plus the chunk that crossed it, the
// request pauses and resumes along the way, and every byte arrives.
export const pipeHonorsDestinationBackpressure = {
  async test(ctrl, env) {
    let stats;
    await withServer(
      (req, res) => {
        const sink = slowSink(16 * 1024);
        let pauses = 0;
        let resumes = 0;
        req.on('pause', () => pauses++);
        req.on('resume', () => resumes++);
        req.pipe(sink);
        sink.on('finish', () => {
          stats = {
            bytes: sink.bytes,
            maxBuffered: sink.maxBuffered,
            largestChunk: sink.largestChunk,
            pauses,
            resumes,
          };
          res.end('ok');
        });
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/', {
          method: 'POST',
          body: new Uint8Array(1024 * 1024),
        });
        strictEqual(await res.text(), 'ok');
        strictEqual(stats.bytes, 1024 * 1024);
        ok(
          stats.maxBuffered <= 16 * 1024 + stats.largestChunk,
          `buffered ${stats.maxBuffered} with chunks up to ${stats.largestChunk}`
        );
        ok(stats.pauses > 0, 'the request was never paused');
        ok(stats.resumes > 0, 'the request was never resumed');
      }
    );
  },
};

// unpipe() after the first chunk: the destination heard 'pipe' and then
// 'unpipe' (with the request), nothing more reaches it and it is not
// ended; the request — left without destinations — is paused, and
// resuming it delivers the rest to a 'data' listener.
export const unpipeStopsDelivery = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    const rest = [];
    let req;
    let dest;
    await withServer(
      (request, res) => {
        req = request;
        dest = recordingSink(events, req);
        req.pipe(dest);
        req.on('end', () => res.end('ok'));
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        await scheduler.wait(20);
        deepStrictEqual(dest.received, ['one']);
        req.unpipe(dest);
        strictEqual(req.isPaused(), true);
        controller.enqueue(enc.encode('two'));
        await scheduler.wait(20);
        deepStrictEqual(dest.received, ['one']);
        req.on('data', (chunk) => rest.push(chunk.toString()));
        req.resume();
        controller.enqueue(enc.encode('three'));
        controller.close();
        strictEqual(await (await pending).text(), 'ok');
        deepStrictEqual(events, ['pipe(true)', 'unpipe(true)']);
        deepStrictEqual(rest, ['two', 'three']);
        strictEqual(dest.writableEnded, false);
      }
    );
  },
};

// A destination that errors is unpiped: no further write reaches it, the
// request is paused (no destinations left) and can be consumed on.
export const erroringDestinationIsUnpiped = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    const rest = [];
    let req;
    let writes = 0;
    await withServer(
      (request, res) => {
        req = request;
        const dest = new Writable({
          write(chunk, encoding, callback) {
            writes++;
            callback(new Error('sink failed'));
          },
        });
        dest.on('error', (err) => events.push(`error(${err.message})`));
        dest.on('unpipe', (src) => events.push(`unpipe(${src === req})`));
        req.pipe(dest);
        req.on('end', () => res.end('ok'));
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        await scheduler.wait(20);
        deepStrictEqual(events, ['unpipe(true)', 'error(sink failed)']);
        strictEqual(req.isPaused(), true);
        controller.enqueue(enc.encode('two'));
        await scheduler.wait(20);
        strictEqual(writes, 1);
        req.on('data', (chunk) => rest.push(chunk.toString()));
        req.resume();
        controller.enqueue(enc.encode('three'));
        controller.close();
        strictEqual(await (await pending).text(), 'ok');
        strictEqual(writes, 1);
        deepStrictEqual(rest, ['two', 'three']);
      }
    );
  },
};

// A source error is not forwarded to the destination, which stays piped
// and open (not ended, not errored): as with any Readable, pipe() leaves
// error handling to the caller, or to pipeline().
export const sourceErrorIsNotForwarded = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    const reqEvents = [];
    let dest;
    const boom = new Error('request failed');
    await withServer(
      (req, res) => {
        dest = recordingSink(events, req);
        req.pipe(dest);
        req.on('error', (err) => reqEvents.push(`error(${err === boom})`));
        req.on('close', () => {
          reqEvents.push('close');
          setTimeout(() => res.end('ok'), 20);
        });
        req.once('data', () => req.destroy(boom));
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        strictEqual(await (await pending).text(), 'ok');
        deepStrictEqual(reqEvents, ['error(true)', 'close']);
        deepStrictEqual(dest.received, ['one']);
        deepStrictEqual(events, ['pipe(true)']);
        strictEqual(dest.writableEnded, false);
        strictEqual(dest.errored, null);
      }
    );
  },
};
