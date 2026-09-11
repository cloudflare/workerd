// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Destroying the IncomingMessage: 'aborted' when the body was not complete,
// 'error' only for listeners, 'close' always; and the body stream underneath
// is cancelled so the producer learns of it.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { withServer, remember, dispatch, manualStream } from 'harness';

const enc = new TextEncoder();

// destroy(err) with an 'error' listener: the error is delivered and the
// handler can still respond.
export const destroyWithErrorEmitsError = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        req.on('error', (err) => {
          res.statusCode = 400;
          res.end(`Request destroyed: ${err.message}`);
        });
        req.destroy(new Error('Destroy test'));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 400);
        strictEqual(await res.text(), 'Request destroyed: Destroy test');
      }
    );
  },
};

// destroy() without an error: 'close' fires, no 'error'.
export const destroyWithoutErrorClosesQuietly = {
  async test(ctrl, env) {
    let errors = 0;
    await withServer(
      (req, res) => {
        req.once('error', () => errors++);
        req.on('close', () => {
          res.statusCode = 200;
          res.end('Request destroyed without error');
        });
        req.destroy();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 200);
        strictEqual(await res.text(), 'Request destroyed without error');
        strictEqual(errors, 0);
      }
    );
  },
};

// destroy(err) with no 'error' listener is swallowed: the request still
// closes and the response can be sent.
export const destroyWithErrorAndNoListenerIsSwallowed = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        req.on('close', () => res.end('closed'));
        req.destroy(new Error('nobody listens'));
      },
      async () => {
        strictEqual(
          await (await env.SERVICE.fetch('http://x/')).text(),
          'closed'
        );
      }
    );
  },
};

// Destroying mid-body (direct dispatch, so the Request's body IS the test's
// stream): 'aborted' and 'close' fire, no more 'data', and the body stream
// is cancelled at once with the destroy reason.
export const destroyMidBodyCancelsBodyStream = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller, cancels } = manualStream();
    const events = [];
    await withServer(
      (req, res) => {
        req.on('data', (chunk) => {
          events.push(`data:${chunk}`);
          req.destroy(new Error('enough'));
        });
        req.on('aborted', () => events.push('aborted'));
        req.on('error', (err) => events.push(`error:${err.message}`));
        req.on('end', () => events.push('end'));
        req.on('close', () => {
          events.push('close');
          setTimeout(() => res.end('done'), 30);
        });
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        await scheduler.wait(20);
        strictEqual(cancels.length, 1);
        strictEqual(cancels[0].message, 'enough');
        strictEqual(await (await pending).text(), 'done');
        deepStrictEqual(events, [
          'data:one',
          'aborted',
          'error:enough',
          'close',
        ]);
      }
    );
  },
};

// destroy() without a reason cancels the body stream with undefined.
export const destroyWithoutReasonCancelsBodyStream = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller, cancels } = manualStream();
    await withServer(
      (req, res) => {
        req.on('data', () => req.destroy());
        req.on('close', () => res.end('done'));
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('one'));
        strictEqual(await (await pending).text(), 'done');
        deepStrictEqual(cancels, [undefined]);
      }
    );
  },
};

// A message whose body was fully read is not cancelled by destroy().
export const destroyAfterCompleteLeavesStreamAlone = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const { stream, controller, cancels } = manualStream();
    await withServer(
      (req, res) => {
        req.resume();
        req.on('end', () => {
          req.destroy();
          res.end(String(req.complete));
        });
      },
      async () => {
        const pending = dispatch(
          new Request('http://x/', { method: 'POST', body: stream })
        );
        controller.enqueue(enc.encode('all'));
        controller.close();
        strictEqual(await (await pending).text(), 'true');
        deepStrictEqual(cancels, []);
      }
    );
  },
};
