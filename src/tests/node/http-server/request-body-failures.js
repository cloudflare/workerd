// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The request body stream failing under the IncomingMessage: an error from
// the stream, or a chunk the message cannot take, aborts the message and
// leaves the response free to be sent. These dispatch in-isolate Requests
// so the body stream is the test's own.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { withServer, remember, dispatch, manualStream } from 'harness';

const enc = new TextEncoder();

function observe(req, res, events) {
  req.on('data', (chunk) => events.push(`data:${chunk}`));
  req.on('aborted', () => events.push('aborted'));
  req.on('error', (err) => events.push(`error:${err.constructor.name}`));
  req.on('end', () => events.push('end'));
  req.on('close', () => {
    events.push(`close:${req.complete}`);
    res.end('done');
  });
}

// The body stream erroring mid-upload: the message is aborted and reports
// the stream's error, then closes incomplete; the handler still answers.
export const bodyStreamErrorAbortsMessage = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    await withServer(
      (req, res) => observe(req, res, events),
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        await scheduler.wait(10);
        controller.error(new Error('upload failed'));
        strictEqual(await (await pending).text(), 'done');
        deepStrictEqual(events, [
          'data:one',
          'aborted',
          'error:Error',
          'close:false',
        ]);
      }
    );
  },
};

// The same while the handler has the message paused: the read pending
// underneath rejects, and the message aborts without having delivered.
export const bodyStreamErrorWhilePausedAbortsMessage = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    await withServer(
      (req, res) => {
        observe(req, res, events);
        req.pause();
        req.read(0);
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        await scheduler.wait(10);
        controller.error(new Error('paused upload failed'));
        strictEqual(await (await pending).text(), 'done');
        deepStrictEqual(events, ['aborted', 'error:Error', 'close:false']);
      }
    );
  },
};

// A chunk the message cannot take — a view over a detached ArrayBuffer —
// aborts it with the conversion's TypeError (the body pump's push() is
// guarded, unlike Readable.fromWeb's in Node).
export const detachedBodyChunkAbortsMessage = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller } = manualStream();
    const events = [];
    await withServer(
      (req, res) => observe(req, res, events),
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        await scheduler.wait(10);
        const buffer = new ArrayBuffer(4);
        const view = new Uint8Array(buffer);
        structuredClone(buffer, { transfer: [buffer] });
        controller.enqueue(view);
        strictEqual(await (await pending).text(), 'done');
        deepStrictEqual(events, [
          'data:one',
          'aborted',
          'error:TypeError',
          'close:false',
        ]);
      }
    );
  },
};
