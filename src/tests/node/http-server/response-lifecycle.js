// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tearing a response down early. Destroyed before the headers are sent, the
// fetch itself fails (with the destroy error, or a premature-close error);
// destroyed after, the body errors the same way. The body stream's cancel
// — a client abandoning the response — destroys the ServerResponse.
//
// Cancellation across the service binding only reaches the peer once the
// exchange completes, so the client-cancel tests dispatch an in-isolate
// Request, whose Response body is the very stream the handler feeds.

import { rejects, strictEqual, deepStrictEqual } from 'node:assert';
import { withServer, remember, dispatch, collectUncaught } from 'harness';

const dec = new TextDecoder();

// destroy(err) before headers: the fetch rejects with that error.
export const destroyWithErrorBeforeHeadersRejectsFetch = {
  async test(ctrl, env) {
    await withServer(
      (req, res) => {
        res.destroy(new Error('gone before headers'));
      },
      async () => {
        await rejects(env.SERVICE.fetch('http://x/'), {
          message: 'gone before headers',
        });
      }
    );
  },
};

// destroy() before headers: the fetch rejects with a premature-close error
// rather than waiting forever.
export const destroyBeforeHeadersRejectsFetch = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('close', () => events.push('close'));
        res.destroy();
      },
      async () => {
        // Only the error's name and message cross the service binding.
        await rejects(env.SERVICE.fetch('http://x/'), {
          name: 'TypeError',
          message: 'Premature close',
        });
        deepStrictEqual(events, ['close']);
      }
    );
  },
};

// destroy(err) after headers: the body errors with that error; 'error'
// then 'close' fire on the response.
export const destroyWithErrorAfterHeadersErrorsBody = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('error', (err) => events.push(`error:${err.message}`));
        res.on('close', () => events.push('close'));
        res.writeHead(200);
        res.write('partial');
        setTimeout(() => res.destroy(new Error('gone after headers')), 10);
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(res.status, 200);
        await rejects(res.text(), { message: 'gone after headers' });
        deepStrictEqual(events, ['error:gone after headers', 'close']);
      }
    );
  },
};

// destroy() after headers: the body ends prematurely (a TypeError with code
// ERR_STREAM_PREMATURE_CLOSE) rather than staying open.
export const destroyAfterHeadersEndsBodyPrematurely = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('error', (err) => events.push(`error:${err.message}`));
        res.on('close', () => events.push('close'));
        res.writeHead(200);
        res.write('partial');
        setTimeout(() => res.destroy(), 10);
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        await rejects(res.text(), {
          name: 'TypeError',
          message: 'Premature close',
        });
        deepStrictEqual(events, ['close']);
      }
    );
  },
};

// The client cancelling the body destroys the response with the cancel
// reason: 'error' and 'close' fire, and later writes fail with
// ERR_STREAM_DESTROYED.
export const clientCancelDestroysResponse = {
  async test(ctrl, env) {
    remember(env, ctrl);
    const events = [];
    let serverRes;
    await withServer(
      (req, res) => {
        serverRes = res;
        res.on('error', (err) => events.push(`error:${err.message}`));
        res.on('close', () => events.push('close'));
        res.writeHead(200);
        res.write('first');
      },
      async () => {
        const res = await dispatch(new Request('http://x/'));
        const reader = res.body.getReader();
        strictEqual(dec.decode((await reader.read()).value), 'first');
        await reader.cancel(new Error('client gone'));
        await scheduler.wait(5);
        deepStrictEqual(events, ['error:client gone', 'close']);
        strictEqual(serverRes.destroyed, true);
        strictEqual(serverRes.errored?.message, 'client gone');
        const err = await new Promise((resolve) =>
          serverRes.write('more', resolve)
        );
        strictEqual(err.code, 'ERR_STREAM_DESTROYED');
      }
    );
  },
};

// A request listener throwing synchronously before any header is sent: the
// response is destroyed with the error ('error', 'close') and the fetch
// rejects with it. Nothing but that error escapes.
export const handlerThrowBeforeHeadersRejectsFetch = {
  async test(ctrl, env) {
    const events = [];
    const boom = new Error('handler boom');
    const leaked = await collectUncaught(() =>
      withServer(
        (req, res) => {
          res.on('error', (err) => events.push(['error', err]));
          res.on('close', () => events.push(['close']));
          throw boom;
        },
        async () => {
          await rejects(env.SERVICE.fetch('http://x/'), {
            message: 'handler boom',
          });
        }
      )
    );
    deepStrictEqual(events, [['error', boom], ['close']]);
    deepStrictEqual(
      leaked.filter((err) => err !== boom),
      []
    );
  },
};

// The same throw after writeHead() and a write(): the Response, ready since
// the first write, is discarded — the fetch rejects with the error — and the
// chunk still in the message buffer when the handler threw is dropped with
// the destroyed response, never enqueued into its errored body.
export const handlerThrowAfterPartialBodyRejectsFetch = {
  async test(ctrl, env) {
    const events = [];
    const boom = new Error('handler boom after write');
    const leaked = await collectUncaught(() =>
      withServer(
        (req, res) => {
          res.on('error', (err) => events.push(['error', err]));
          res.on('close', () => events.push(['close']));
          res.writeHead(200, { 'Content-Type': 'text/plain' });
          res.write('partial');
          throw boom;
        },
        async () => {
          await rejects(env.SERVICE.fetch('http://x/'), {
            message: 'handler boom after write',
          });
        }
      )
    );
    deepStrictEqual(events, [['error', boom], ['close']]);
    deepStrictEqual(
      leaked.filter((err) => err !== boom),
      []
    );
  },
};
