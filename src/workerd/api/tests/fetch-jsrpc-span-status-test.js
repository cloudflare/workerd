// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Failed fetches and JS RPC calls, whose spans fetch-jsrpc-span-status-test-tail.js checks.

import assert from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

class NotSerializable {}

export class Backend extends WorkerEntrypoint {
  async fetch(request) {
    switch (new URL(request.url).pathname) {
      case '/ok':
        return new Response('ok');
      case '/not-found':
        return new Response('not found', { status: 404 });
      case '/error':
        return new Response('error', { status: 500 });
      case '/redirect':
        return new Response(null, {
          status: 302,
          headers: { location: '/bad-redirect' },
        });
      case '/bad-redirect':
        return new Response(null, {
          status: 302,
          headers: { location: 'http://[' },
        });
      case '/slow':
        await scheduler.wait(10000);
        return new Response('slow');
    }
    return new Response('bad request', { status: 400 });
  }

  ok() {
    return 'ok';
  }

  async rejects() {
    throw new Error('method failed');
  }

  throwsSync() {
    throw new TypeError('method failed synchronously');
  }

  returnsUnserializable() {
    return new NotSerializable();
  }
}

// Makes the calls. They're made in this invocation, rather than in test(), so the tail worker gets
// its events before its own test() checks them.
export class Caller extends WorkerEntrypoint {
  async run() {
    const env = this.env;

    // fetch
    assert.strictEqual(
      (await env.BACKEND.fetch('http://backend/ok')).status,
      200
    );
    assert.strictEqual(
      (await env.BACKEND.fetch('http://backend/not-found')).status,
      404
    );
    assert.strictEqual(
      (await env.BACKEND.fetch('http://backend/error')).status,
      500
    );
    await assert.rejects(
      async () => env.UNREACHABLE.fetch('http://unreachable/'),
      { message: 'Network connection lost.' }
    );
    {
      const controller = new AbortController();
      const promise = env.BACKEND.fetch('http://backend/slow', {
        signal: controller.signal,
      });
      controller.abort(new Error('fetch aborted'));
      await assert.rejects(promise, { message: 'fetch aborted' });
    }
    // The first hop succeeds, and the second fails to follow the redirect.
    await assert.rejects(
      async () => env.BACKEND.fetch('http://backend/redirect'),
      {
        name: 'TypeError',
        message: 'Invalid Location header; unable to follow redirect.',
      }
    );

    // JS RPC
    assert.strictEqual(await env.BACKEND.ok(), 'ok');
    await assert.rejects(async () => env.BACKEND.rejects(), {
      name: 'Error',
      message: 'method failed',
    });
    await assert.rejects(async () => env.BACKEND.throwsSync(), {
      name: 'TypeError',
      message: 'method failed synchronously',
    });
    await assert.rejects(async () => env.BACKEND.returnsUnserializable(), {
      name: 'DataCloneError',
    });
  }
}

export const test = {
  async test(ctrl, env) {
    await env.CALLER.run();
  },
};
