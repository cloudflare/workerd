// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test: a fire-and-forget `env.QUEUE.send()` followed by a 3xx
// response used to crash workerd inside `IoContext_IncomingRequest::drain()`
// because the loopback subrequest's `WorkerEntrypoint` was freed while its
// `requestImpl` coroutine was still running.

import assert from 'node:assert';

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);

    if (pathname === '/message') {
      return Response.json({
        metadata: {
          metrics: {
            backlogCount: 0,
            backlogBytes: 0,
            oldestMessageTimestamp: 0,
          },
        },
      });
    }

    if (pathname === '/queue-then-redirect') {
      env.QUEUE.send({ hello: 'world' });
      return new Response(null, {
        status: 302,
        headers: { location: '/anywhere' },
      });
    }

    return new Response('not found', { status: 404 });
  },

  async test(_ctrl, env) {
    const resp = await env.SERVICE.fetch(
      'http://example.com/queue-then-redirect',
      { redirect: 'manual' }
    );
    assert.strictEqual(resp.status, 302);
    assert.strictEqual(resp.headers.get('location'), '/anywhere');
  },
};
